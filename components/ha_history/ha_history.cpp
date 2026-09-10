#include "ha_history.h"

#include <algorithm>
#include <ctime>

#include "esphome/components/json/json_util.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace ha_history {

static const char *const TAG = "ha_history";

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------

uint32_t HaHistory::now_utc() const {
  if (this->time_ == nullptr)
    return 0;
  auto t = this->time_->now();
  if (!t.is_valid())
    return 0;
  return (uint32_t) t.timestamp;
}

uint32_t HaHistory::window_start_for_(const HaHistorySensor *s, uint32_t now) const {
  if (s->window_kind_ == WindowKind::TODAY) {
    // Local midnight, expressed as a UTC epoch. The device's zone is Home
    // Assistant's when `time: platform: homeassistant` is used, which is why
    // the README asks for it.
    auto t = this->time_->now();
    t.hour = 0;
    t.minute = 0;
    t.second = 0;
    t.recalc_timestamp_local();
    return (uint32_t) t.timestamp;
  }
  return now > s->window_s_ ? now - s->window_s_ : 0;
}

void HaHistory::iso_utc_(uint32_t epoch, char *out, size_t len) {
  time_t t = epoch;
  struct tm tm_utc;
  gmtime_r(&t, &tm_utc);
  strftime(out, len, "%Y-%m-%dT%H:%M:%S+00:00", &tm_utc);
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------

void HaHistory::loop() {
  const uint32_t now = this->now_utc();
  if (now == 0) {
    // Nothing can be bucketed or requested without a clock. Say so, but not
    // every loop.
    const uint32_t ms = millis();
    if (ms - this->clock_wait_logged_ms_ > 30000) {
      this->clock_wait_logged_ms_ = ms;
      ESP_LOGD(TAG, "waiting for a valid clock (time: platform: homeassistant syncs once HA connects)");
    }
    return;
  }
  if (!this->clock_seen_) {
    this->clock_seen_ = true;
    ESP_LOGD(TAG, "clock valid; bucketing and backfill can start");
  }
  this->tick_buckets_(now);
  this->tick_backfill_(millis(), now);
}

void HaHistory::tick_buckets_(uint32_t now) {
  const auto local = this->time_->now();

  for (auto *s : this->sensors_) {
    const uint32_t cur = align_down(now, s->bucket_s_);
    s->window_start_ = this->window_start_for_(s, now);

    // `today`: the buffer is the calendar day, so it empties at local midnight.
    if (s->window_kind_ == WindowKind::TODAY) {
      if (s->last_local_day_ >= 0 && local.day_of_year != s->last_local_day_) {
        ESP_LOGD(TAG, "'%s': new day, clearing history", s->entity_id_);
        s->buffer_.clear();
        s->update_trigger_.trigger();
      }
      s->last_local_day_ = local.day_of_year;
    }

    if (!s->acc_.is_open()) {
      // First bucket after boot. Seeded with whatever Home Assistant has pushed
      // so far, if anything; the seam repair fixes this bucket up later either
      // way, because a boot-time partial bucket cannot match HA's full one.
      s->acc_.begin(cur, s->bucket_s_, s->statistic_, s->last_seen_);
      s->boot_bucket_start_ = cur;
      continue;
    }

    if (s->acc_.start() != cur) {
      Point p;
      if (s->acc_.close(&p))
        s->buffer_.put(p);
      // Carry the value in force across the boundary, as HA does, so a sensor
      // that reports rarely still fills every bucket.
      s->acc_.begin(cur, s->bucket_s_, s->statistic_, s->acc_.last_value());
      if (s->window_kind_ == WindowKind::ROLLING)
        s->buffer_.drop_before(align_down(s->window_start_, s->bucket_s_));
      s->update_trigger_.trigger();
    }
  }
}

// ---------------------------------------------------------------------------
// Backfill
// ---------------------------------------------------------------------------

void HaHistory::tick_backfill_(uint32_t now_ms, uint32_t now) {
  const bool connected = api::global_api_server != nullptr && api::global_api_server->is_connected();

  if (connected != this->api_was_connected_) {
    this->api_was_connected_ = connected;
    this->probe_fail_since_ms_ = 0;
    this->probe_warned_ = false;
    ESP_LOGD(TAG, "API %s", connected ? "connected" : "disconnected");
    if (!connected) {
      this->disconnected_ms_ = now_ms;
      if (this->pending_ != nullptr) {
        // A connection that dies right after we asked is the signature of a
        // reply that exceeded the API frame limit. Back off hard rather than
        // re-asking the moment HA reconnects.
        const bool right_after = now_ms - this->pending_->sent_ms < TOO_LARGE_WINDOW_MS;
        this->fail_pending_(right_after ? "connection dropped right after the request" : "connection lost",
                            right_after);
      }
    } else {
      // New connection: attempt counters are per connection. A sensor that gave
      // up gets another go, and a gap longer than a bucket is repaired by
      // simply loading again - backfilled buckets win where both exist.
      const uint32_t outage_ms = this->disconnected_ms_ ? now_ms - this->disconnected_ms_ : 0;
      for (auto *s : this->sensors_) {
        s->attempts_ = 0;
        s->timeouts_ = 0;
        s->backoff_ms_ = 0;
        s->next_try_ms_ = now_ms;
        if (s->load_state_ == LoadState::GIVEN_UP)
          s->load_state_ = LoadState::IDLE;
        if (s->load_state_ == LoadState::LOADED && outage_ms > s->bucket_s_ * 1000UL) {
          ESP_LOGD(TAG, "'%s': API was down %us, reloading history", s->entity_id_, (unsigned) (outage_ms / 1000));
          s->load_state_ = LoadState::IDLE;
        }
      }
    }
  }
  if (!connected)
    return;

  if (this->pending_ != nullptr) {
    if (now_ms - this->pending_->sent_ms > DEADLINE_MS)
      this->fail_pending_("no reply", false);
    else
      return;  // one request in flight at a time
  }

  for (auto *s : this->sensors_) {
    if (s->load_state_ == LoadState::IDLE) {
      if ((int32_t) (now_ms - s->next_try_ms_) < 0)
        continue;
      if (!this->try_send_(s, now, false)) {
        // Home Assistant has not subscribed to actions on this connection yet.
        // It does so within a second or two of authenticating; much longer than
        // that and something is wrong, so say so once.
        s->next_try_ms_ = now_ms + 1000;
        if (this->probe_fail_since_ms_ == 0) {
          this->probe_fail_since_ms_ = now_ms;
        } else if (!this->probe_warned_ && now_ms - this->probe_fail_since_ms_ > 10000) {
          this->probe_warned_ = true;
          ESP_LOGW(TAG, "no connected API client has subscribed to Home Assistant actions for %us; "
                        "history cannot be requested until one does",
                   (unsigned) ((now_ms - this->probe_fail_since_ms_) / 1000));
        }
      } else {
        this->probe_fail_since_ms_ = 0;
      }
      return;
    }
    if (s->load_state_ == LoadState::LOADED && !s->seam_done_ && now_ms - s->loaded_ms_ >= SEAM_DELAY_MS) {
      if (this->try_send_(s, now, true))
        return;
      s->loaded_ms_ = now_ms;  // not subscribed yet; ask again in a while
      return;
    }
  }
}

std::string HaHistory::build_template_(const HaHistorySensor *s, uint32_t anchor) const {
  // Rendered by Home Assistant with `response` in scope (strict mode), so:
  //  * `.get(id, [])` rather than `[id]` - HA omits the key entirely for an
  //    entity with no statistics, and strict mode would raise on it;
  //  * indices relative to the anchor, so the device never parses a timestamp
  //    and the payload is the same size on any date;
  //  * a flat [idx, val, idx, val] list - half the ArduinoJson nodes of nested
  //    pairs, which matters on a board without PSRAM.
  const char *stat = to_string(s->statistic_);
  char buf[512];
  snprintf(buf, sizeof(buf),
           "{%% set rows = response.statistics.get('%s', []) %%}"
           "[{%% for r in rows if r.%s is not none -%%}"
           "{{ ((as_timestamp(r.start) - %u) / %u) | int }},{{ r.%s | round(4) }}{{ ',' if not loop.last }}"
           "{%%- endfor %%}]",
           s->entity_id_, stat, (unsigned) anchor, (unsigned) s->bucket_s_, stat);
  return std::string(buf);
}

bool HaHistory::try_send_(HaHistorySensor *s, uint32_t now, bool seam) {
  // Both sides index buckets from a boundary-aligned anchor. Local midnight is
  // 5-minute aligned everywhere but not hour-aligned in half-hour zones.
  const uint32_t anchor = seam ? s->boot_bucket_start_ : align_down(this->window_start_for_(s, now), s->bucket_s_);

  char start_iso[40], end_iso[40];
  iso_utc_(anchor, start_iso, sizeof(start_iso));
  iso_utc_(now, end_iso, sizeof(end_iso));

  // Backing strings must outlive send_homeassistant_action(); the request holds
  // StringRefs into them. Plain strings throughout: recorder.get_statistics
  // wraps `statistic_ids` and `types` with ensure_list itself.
  const std::string service = "recorder.get_statistics";
  const std::string k_start = "start_time", k_end = "end_time", k_period = "period", k_types = "types",
                    k_ids = "statistic_ids";
  const std::string v_start = start_iso, v_end = end_iso, v_period = s->bucket_s_ == 3600 ? "hour" : "5minute",
                    v_types = to_string(s->statistic_), v_ids = s->entity_id_;
  const std::string tmpl = this->build_template_(s, anchor);

  api::HomeassistantActionRequest req;
  req.service = StringRef(service);
  req.data.init(5);
  const std::string *kv[5][2] = {
      {&k_start, &v_start}, {&k_end, &v_end}, {&k_period, &v_period}, {&k_types, &v_types}, {&k_ids, &v_ids},
  };
  for (auto &pair : kv) {
    auto &m = req.data.emplace_back();
    m.key = StringRef(*pair[0]);
    m.value = StringRef(*pair[1]);
  }
  const uint32_t call_id = this->call_id_next_++;
  req.call_id = call_id;
  req.wants_response = true;
  req.response_template = StringRef(tmpl);

  // One client, and only one that has subscribed to actions. `false` from every
  // client means HA has not subscribed yet on this connection - nothing was
  // sent, so nothing is registered and nothing can leak.
  bool sent = false;
  unsigned clients = 0;
  for (auto &client : api::global_api_server->active_clients()) {
    clients++;
    if (client->send_homeassistant_action(req)) {
      sent = true;
      break;
    }
  }
  if (!sent) {
    ESP_LOGV(TAG, "'%s': %u API client(s), none subscribed to actions yet", s->entity_id_, clients);
    return false;
  }

  auto p = std::make_shared<Pending>(Pending{s, call_id, millis(), anchor, s->bucket_s_, seam});
  this->pending_ = p;
  std::weak_ptr<Pending> weak = p;
  api::global_api_server->register_action_response_callback(call_id, [this, weak](const api::ActionResponse &r) {
    auto sp = weak.lock();
    if (sp == nullptr)
      return;  // expired or superseded; HA's late reply is ignored
    this->handle_response_(sp, r);
  });

  s->load_state_ = LoadState::PENDING;
  s->attempts_++;
  ESP_LOGD(TAG, "'%s': requesting %s statistics from %s (%s)%s", s->entity_id_, v_period.c_str(), start_iso,
           to_string(s->statistic_), seam ? " [seam repair]" : "");
  return true;
}

void HaHistory::handle_response_(const std::shared_ptr<Pending> &p, const api::ActionResponse &r) {
  if (this->pending_ != p)
    return;
  this->pending_.reset();
  auto *s = p->sensor;

  if (!r.is_success()) {
    // HA reached the service and it failed: a missing recorder, a template
    // error, a validation error. Real information, so say it verbatim.
    ESP_LOGW(TAG, "'%s': Home Assistant returned an error: %s", s->entity_id_, r.get_error_message().c_str());
    this->fail_after_error_(s, p->seam);
    return;
  }

  JsonObjectConst root = r.get_json();
  JsonVariantConst v = root["response"];
  JsonArrayConst arr;
  JsonDocument fallback;
  if (v.is<JsonArrayConst>()) {
    arr = v.as<JsonArrayConst>();
  } else if (v.is<const char *>()) {
    // HA normally literal-evals the rendered list into a native array before
    // JSON-encoding it; if it ever arrives as a string, parse that.
    fallback = json::parse_json(std::string(v.as<const char *>()));
    arr = fallback.as<JsonArrayConst>();
  }

  size_t n = 0;
  bool have_idx = false;
  long idx = 0;
  for (JsonVariantConst e : arr) {
    if (!have_idx) {
      idx = e.as<long>();
      have_idx = true;
    } else {
      if (put_indexed(s->buffer_, p->anchor, p->bucket_s, idx, e.as<float>()))
        n++;
      have_idx = false;
    }
  }

  s->load_state_ = LoadState::LOADED;
  s->attempts_ = 0;
  s->timeouts_ = 0;
  s->backoff_ms_ = 0;
  s->last_loaded_points_ = n;
  if (p->seam) {
    s->seam_done_ = true;
  } else {
    s->loaded_ms_ = millis();
  }
  if (n == 0 && !p->seam) {
    ESP_LOGW(TAG, "'%s': Home Assistant has no statistics for this entity in the window. "
                  "Statistics need a state_class (measurement, total or total_increasing).",
             s->entity_id_);
  } else {
    ESP_LOGI(TAG, "'%s': loaded %u points%s, %u held", s->entity_id_, (unsigned) n, p->seam ? " (seam repair)" : "",
             (unsigned) s->buffer_.size());
  }
  s->loaded_trigger_.trigger();
}

void HaHistory::fail_after_error_(HaHistorySensor *s, bool seam) {
  if (seam) {
    s->seam_done_ = true;
    s->load_state_ = LoadState::LOADED;
    return;
  }
  s->backoff_ms_ = s->backoff_ms_ == 0 ? this->retry_ms_ : std::min(s->backoff_ms_ * 2, MAX_BACKOFF_MS);
  s->next_try_ms_ = millis() + s->backoff_ms_;
  s->load_state_ = s->attempts_ >= MAX_ATTEMPTS ? LoadState::GIVEN_UP : LoadState::IDLE;
}

void HaHistory::fail_pending_(const char *why, bool too_large) {
  auto p = this->pending_;
  this->pending_.reset();
  if (p == nullptr)
    return;
  auto *s = p->sensor;

  if (p->seam) {
    // The seam repair is a nicety; do not fight for it.
    ESP_LOGD(TAG, "'%s': seam repair skipped (%s)", s->entity_id_, why);
    s->seam_done_ = true;
    s->load_state_ = LoadState::LOADED;
    return;
  }

  s->timeouts_++;
  if (too_large) {
    s->backoff_ms_ = MAX_BACKOFF_MS;
    ESP_LOGW(TAG, "'%s': %s. If this repeats, the statistics reply is probably exceeding the API frame limit - "
                  "use a shorter window or bucket: hour. Backing off %us.",
             s->entity_id_, why, (unsigned) (MAX_BACKOFF_MS / 1000));
  } else {
    s->backoff_ms_ = s->backoff_ms_ == 0 ? this->retry_ms_ : std::min(s->backoff_ms_ * 2, MAX_BACKOFF_MS);
    if (s->timeouts_ >= 2) {
      // Home Assistant sends NOTHING when actions are disallowed for a device -
      // no error, no response - so silence is the only symptom there is.
      ESP_LOGW(TAG, "'%s': %s from Home Assistant (%u in a row). Is 'Allow the device to perform Home Assistant "
                    "actions' enabled for this device in the ESPHome integration's options? Retrying in %us.",
               s->entity_id_, why, (unsigned) s->timeouts_, (unsigned) (s->backoff_ms_ / 1000));
    } else {
      ESP_LOGD(TAG, "'%s': %s, retrying in %us", s->entity_id_, why, (unsigned) (s->backoff_ms_ / 1000));
    }
  }
  s->next_try_ms_ = millis() + s->backoff_ms_;

  if (s->attempts_ >= MAX_ATTEMPTS) {
    ESP_LOGW(TAG, "'%s': giving up on history until the API reconnects or it is reloaded", s->entity_id_);
    s->load_state_ = LoadState::GIVEN_UP;
  } else {
    s->load_state_ = LoadState::IDLE;
  }
}

void HaHistory::request_backfill() {
  ESP_LOGI(TAG, "reloading history for %u sensor(s)", (unsigned) this->sensors_.size());
  this->pending_.reset();  // any in-flight reply becomes a stale weak_ptr
  for (auto *s : this->sensors_) {
    s->load_state_ = LoadState::IDLE;
    s->attempts_ = 0;
    s->timeouts_ = 0;
    s->backoff_ms_ = 0;
    s->next_try_ms_ = millis();
    s->seam_done_ = false;
  }
}

void HaHistory::dump_config() {
  ESP_LOGCONFIG(TAG, "Home Assistant history:");
  ESP_LOGCONFIG(TAG, "  Sensors: %u", (unsigned) this->sensors_.size());
  ESP_LOGCONFIG(TAG, "  Retry interval: %us, backing off to %us", (unsigned) (this->retry_ms_ / 1000),
                (unsigned) (MAX_BACKOFF_MS / 1000));
}

void HaHistoryReloadButton::dump_config() { LOG_BUTTON("", "Home Assistant history reload", this); }

}  // namespace ha_history
}  // namespace esphome
