#include "action_client.h"

#ifdef USE_API

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace ha_api_core {

// Non-zero, and far from the counters the built-in homeassistant.action uses -
// those are function-local statics per template instantiation, starting at 1.
uint32_t ActionClient::call_id_next_ = 0x48480000;

const char *to_string(FailReason reason) {
  switch (reason) {
    case FailReason::TIMEOUT:
      return "no reply";
    case FailReason::DISCONNECTED:
      return "connection lost";
    case FailReason::TOO_LARGE:
      return "connection dropped right after the request";
  }
  return "unknown";
}

void ActionClient::loop() {
  const uint32_t now_ms = millis();
  const bool connected = api::global_api_server != nullptr && api::global_api_server->is_connected();

  if (connected != this->connected_) {
    this->connected_ = connected;
    this->probe_fail_since_ms_ = 0;
    this->probe_warned_ = false;
    ESP_LOGD(this->tag_, "API %s", connected ? "connected" : "disconnected");

    if (!connected) {
      this->disconnected_ms_ = now_ms;
      if (this->pending_ != nullptr) {
        // A connection that dies moments after we asked is the signature of a
        // reply that exceeded the API frame limit. Telling the two apart
        // matters: the caller must back off hard for one and may retry the
        // other, and re-asking after a too-large reply is a flap loop.
        const bool right_after = now_ms - this->pending_->sent_ms < TOO_LARGE_WINDOW_MS;
        this->fail_pending_(right_after ? FailReason::TOO_LARGE : FailReason::DISCONNECTED);
      }
      if (this->on_api_change_)
        this->on_api_change_(false, 0);
    } else {
      const uint32_t outage_ms = this->disconnected_ms_ != 0 ? now_ms - this->disconnected_ms_ : 0;
      if (this->on_api_change_)
        this->on_api_change_(true, outage_ms);
    }
  }

  if (this->pending_ != nullptr && now_ms - this->pending_->sent_ms > this->deadline_ms_)
    this->fail_pending_(FailReason::TIMEOUT);
}

bool ActionClient::send(const ActionRequest &req, const char *what, ReplyCb on_reply, FailCb on_fail) {
  if (api::global_api_server == nullptr || req.action == nullptr)
    return false;

  // Burned even when the send is refused, so a call_id is never reused across
  // retries: handle_action_response pairs the FIRST entry with a matching id,
  // and a reused id would deliver a fresh reply to a stale callback.
  const uint32_t call_id = ActionClient::call_id_next_++;

  api::HomeassistantActionRequest msg;
  msg.service = StringRef(req.action);
  auto fill = [](auto &dest, const std::vector<std::pair<const char *, std::string>> &src) {
    dest.init(src.size());
    for (const auto &kv : src) {
      auto &m = dest.emplace_back();
      m.key = StringRef(kv.first);
      m.value = StringRef(kv.second);
    }
  };
  fill(msg.data, req.data);
  fill(msg.data_template, req.data_template);
  fill(msg.variables, req.variables);
  msg.call_id = call_id;
  msg.wants_response = true;
  if (!req.response_template.empty())
    msg.response_template = StringRef(req.response_template);

  // One client, and only one that has subscribed to actions. False from every
  // client means Home Assistant has not subscribed yet on this connection:
  // nothing was sent, so nothing is registered and nothing can leak.
  bool sent = false;
  unsigned clients = 0;
  for (auto &client : api::global_api_server->active_clients()) {
    clients++;
    if (client->send_homeassistant_action(msg)) {
      sent = true;
      break;
    }
  }
  if (!sent) {
    ESP_LOGV(this->tag_, "%s: %u API client(s), none subscribed to actions yet", what, clients);
    this->note_probe_failure_(millis());
    return false;
  }
  this->probe_fail_since_ms_ = 0;

  auto p = std::make_shared<Pending>();
  p->call_id = call_id;
  p->sent_ms = millis();
  p->what = what;
  p->on_reply = std::move(on_reply);
  p->on_fail = std::move(on_fail);
  this->pending_ = p;

  // The weak_ptr is what makes expiry safe without a removal API: once the
  // client has moved on, lock() fails and Home Assistant's late reply is
  // dropped. Home Assistant's reply still erases the registry entry, so only a
  // reply that never arrives leaves one behind.
  std::weak_ptr<Pending> weak = p;
  api::global_api_server->register_action_response_callback(call_id, [this, weak](const api::ActionResponse &r) {
    auto sp = weak.lock();
    if (sp == nullptr || this->pending_ != sp)
      return;
    this->pending_.reset();
    sp->on_reply(r);
  });
  return true;
}

void ActionClient::note_probe_failure_(uint32_t now_ms) {
  if (this->probe_fail_since_ms_ == 0) {
    this->probe_fail_since_ms_ = now_ms;
  } else if (!this->probe_warned_ && now_ms - this->probe_fail_since_ms_ > PROBE_WARN_MS) {
    this->probe_warned_ = true;
    ESP_LOGW(this->tag_,
             "no connected API client has subscribed to Home Assistant actions for %us; "
             "nothing can be requested until one does",
             (unsigned) ((now_ms - this->probe_fail_since_ms_) / 1000));
  }
}

void ActionClient::fail_pending_(FailReason reason) {
  auto p = this->pending_;
  this->pending_.reset();
  if (p == nullptr)
    return;
  if (p->on_fail)
    p->on_fail(reason);
}

void ActionClient::cancel() { this->pending_.reset(); }

}  // namespace ha_api_core
}  // namespace esphome

#endif  // USE_API
