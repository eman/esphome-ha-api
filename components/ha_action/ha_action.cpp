#include "ha_action.h"

#ifdef USE_API

#include <algorithm>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace ha_action {

static const char *const TAG = "ha_action";

std::string Target::path_str() const {
  std::string out;
  for (const auto &t : this->path_) {
    if (t.is_index()) {
      out += "[" + std::to_string(t.index) + "]";
    } else {
      if (!out.empty())
        out += ".";
      out += t.key;
    }
  }
  return out;
}

HaAction::HaAction() : client_(TAG) {}

void HaAction::setup() {
  this->client_.set_on_api_change([this](bool connected, uint32_t outage_ms) {
    this->on_api_change_(connected, outage_ms);
  });
}

void HaAction::on_api_change_(bool connected, uint32_t outage_ms) {
  (void) outage_ms;
  if (!connected)
    return;
  // Attempt counters are per connection: a request that gave up gets another go
  // once Home Assistant is back, and the first fetch of a connection is
  // immediate rather than waiting out a whole update_interval.
  this->attempts_ = 0;
  this->timeouts_ = 0;
  this->backoff_ms_ = 0;
  this->next_due_ms_ = millis();
  this->due_ = true;
}

void HaAction::loop() {
  this->client_.loop();
  if (!this->client_.connected() || this->client_.busy() || !this->due_)
    return;
  if ((int32_t) (millis() - this->next_due_ms_) < 0)
    return;

  // A false send means Home Assistant has not subscribed to actions on this
  // connection yet, which is the normal state for the first moments after boot.
  // Nothing was sent and nothing registered, so just come back shortly; the
  // client warns on its own if the wait becomes unreasonable.
  if (!this->try_send_())
    this->next_due_ms_ = millis() + 1000;
}

bool HaAction::try_send_() {
  // Everything the request refers to must outlive the send, because the
  // protobuf message holds StringRefs into it. Evaluating the templates into
  // `req` and sending from the same scope is what guarantees that.
  ActionRequest req;
  req.action = this->action_;
  req.data.reserve(this->data_.size());
  for (auto &kv : this->data_)
    req.data.emplace_back(kv.first, kv.second.value());
  if (this->has_template_)
    req.response_template = this->response_template_.value();

  const bool sent = this->client_.send(
      req, this->name_, [this](const api::ActionResponse &r) { this->handle_reply_(r); },
      [this](FailReason reason) { this->fail_(reason); });
  if (!sent)
    return false;

  this->due_ = false;
  this->attempts_++;
  ESP_LOGD(TAG, "'%s': calling %s", this->name_, this->action_);
  return true;
}

JsonVariantConst HaAction::unwrap_(JsonObjectConst root, JsonDocument &scratch) const {
  // Home Assistant wraps the payload in {"response": ...} whether or not a
  // response_template was used (manager.py), so this is unconditional.
  JsonVariantConst payload = root["response"];

  // A rendered template is literal_eval'd by Home Assistant before it is JSON
  // encoded, so `[1, 2]` and `{'a': 1}` arrive as a real array and object. But
  // literal_eval does not know JSON's `true`/`false`/`null`, so a template
  // ending in `| tojson` over anything containing a boolean comes back as a
  // STRING holding JSON. Parse that rather than making the user care.
  if (payload.is<const char *>()) {
    const char *s = payload.as<const char *>();
    if (s != nullptr && (s[0] == '[' || s[0] == '{')) {
      if (deserializeJson(scratch, s) == DeserializationError::Ok)
        return scratch.as<JsonVariantConst>();
    }
  }
  return payload;
}

void HaAction::handle_reply_(const api::ActionResponse &r) {
  if (!r.is_success()) {
    // Home Assistant reached the action and it failed: an unknown action, a
    // validation error, a template error. Real information, so say it verbatim.
    const std::string err = r.get_error_message().str();
    ESP_LOGW(TAG, "'%s': Home Assistant returned an error: %s", this->name_, err.c_str());
    this->error_trigger_.trigger(err);
    this->fail_after_error_();
    return;
  }

  JsonDocument scratch;
  JsonVariantConst payload = this->unwrap_(r.get_json(), scratch);

  for (auto *target : this->targets_)
    target->publish_from(payload);

  if (this->retain_) {
    // ArduinoJson deep-copies here, which is the point: the reply's document
    // lives on the receive path's stack and is gone the moment this returns.
    this->retained_.set(payload);
    if (this->retained_.overflowed())
      ESP_LOGW(TAG, "'%s': response did not fit the retained document; it was truncated", this->name_);
  }

  this->response_trigger_.trigger(payload);

  this->have_response_ = true;
  this->attempts_ = 0;
  this->timeouts_ = 0;
  this->backoff_ms_ = 0;
  if (this->update_interval_ms_ != 0) {
    this->next_due_ms_ = millis() + this->update_interval_ms_;
    this->due_ = true;
  }
  ESP_LOGD(TAG, "'%s': response applied to %u entit%s", this->name_, (unsigned) this->targets_.size(),
           this->targets_.size() == 1 ? "y" : "ies");
}

void HaAction::fail_(FailReason reason) {
  const char *why = to_string(reason);

  if (reason == FailReason::TOO_LARGE) {
    this->backoff_ms_ = MAX_BACKOFF_MS;
    ESP_LOGW(TAG, "'%s': %s. If this repeats, the reply is probably exceeding the 32 KiB API frame - "
                  "narrow it with a response_template. Backing off %us.",
             this->name_, why, (unsigned) (MAX_BACKOFF_MS / 1000));
    this->next_due_ms_ = millis() + this->backoff_ms_;
    this->due_ = true;
    return;
  }

  // Only silence counts toward this diagnostic. A dropped connection explains
  // itself; Home Assistant sending NOTHING - no error, no response - when
  // actions are disallowed for a device is the one failure mode with no other
  // symptom.
  if (reason == FailReason::TIMEOUT)
    this->timeouts_++;
  this->schedule_retry_(why);
}

void HaAction::fail_after_error_() { this->schedule_retry_("action failed"); }

void HaAction::schedule_retry_(const char *why) {
  this->backoff_ms_ = this->backoff_ms_ == 0 ? this->retry_ms_ : std::min(this->backoff_ms_ * 2, MAX_BACKOFF_MS);

  if (this->timeouts_ >= 2) {
    ESP_LOGW(TAG, "'%s': %s from Home Assistant (%u in a row). Is 'Allow the device to perform Home Assistant "
                  "actions' enabled for this device in the ESPHome integration's options? Retrying in %us.",
             this->name_, why, (unsigned) this->timeouts_, (unsigned) (this->backoff_ms_ / 1000));
  } else {
    ESP_LOGD(TAG, "'%s': %s, retrying in %us", this->name_, why, (unsigned) (this->backoff_ms_ / 1000));
  }

  this->next_due_ms_ = millis() + this->backoff_ms_;
  this->due_ = true;
  if (this->attempts_ >= MAX_ATTEMPTS) {
    ESP_LOGW(TAG, "'%s': giving up until the API reconnects or it is refreshed", this->name_);
    this->due_ = false;
  }
}

void HaAction::refresh() {
  ESP_LOGI(TAG, "'%s': refreshing", this->name_);
  this->client_.cancel();  // any in-flight reply becomes a stale weak_ptr
  this->attempts_ = 0;
  this->timeouts_ = 0;
  this->backoff_ms_ = 0;
  this->next_due_ms_ = millis();
  this->due_ = true;
}

void HaAction::dump_config() {
  ESP_LOGCONFIG(TAG, "Home Assistant action '%s':", this->name_);
  ESP_LOGCONFIG(TAG, "  Action: %s", this->action_);
  if (this->update_interval_ms_ == 0) {
    ESP_LOGCONFIG(TAG, "  Update interval: once per API connection");
  } else {
    ESP_LOGCONFIG(TAG, "  Update interval: %us", (unsigned) (this->update_interval_ms_ / 1000));
  }
  if (this->has_template_)
    ESP_LOGCONFIG(TAG, "  Response is reshaped by a server-side template");
  if (this->retain_)
    ESP_LOGCONFIG(TAG, "  Retaining the parsed response");
  // Paths are printed so a typo is visible at boot, without waiting for a reply
  // that then silently publishes nothing.
  for (auto *target : this->targets_)
    ESP_LOGCONFIG(TAG, "  Path: %s", target->path_str().c_str());
}

#ifdef USE_BUTTON
void HaActionRefreshButton::dump_config() { LOG_BUTTON("", "Home Assistant action refresh", this); }
#endif

}  // namespace ha_action
}  // namespace esphome

#endif  // USE_API
