#pragma once

// The hub: turns live Home Assistant pushes into buckets, and fills each
// sensor's buffer with Home Assistant's recorder statistics at startup.
//
// The backfill rides the native API connection Home Assistant already holds to
// the device: the hub sends a `recorder.get_statistics` action with a
// `response_template`, Home Assistant renders that Jinja server-side into a
// compact list of (bucket index, value) pairs, and the reply arrives through
// the shared ha_api_core::ActionClient. No HTTP client, no TLS, no long-lived
// token.
//
// The wire mechanics - the boot race, request expiry, the frame-size guard,
// call_id allocation - live in ha_api_core/action_client.h, which documents why
// each one is needed. What stays here is the part specific to history: which
// sensor to ask about next, and how hard to retry when Home Assistant says
// nothing at all.

#include "esphome/core/defines.h"

#include <string>
#include <vector>

#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif
#include "esphome/components/ha_api_core/action_client.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/core/component.h"

#include "ha_history_sensor.h"

namespace esphome {
namespace ha_history {

class HaHistory : public Component {
 public:
  HaHistory();

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  void set_time(time::RealTimeClock *t) { this->time_ = t; }
  void set_retry_interval(uint32_t ms) { this->retry_ms_ = ms; }
  void register_sensor(HaHistorySensor *s) { this->sensors_.push_back(s); }

  /// Restarts the backfill for every sensor. Bound to the reload button; the
  /// case it exists for is "the user just enabled actions in Home Assistant".
  void request_backfill();

  /// Current UTC epoch, or 0 while the clock is not yet valid.
  uint32_t now_utc() const;

 protected:
  void tick_buckets_(uint32_t now);
  void tick_backfill_(uint32_t now_ms, uint32_t now);
  void on_api_change_(bool connected, uint32_t outage_ms);
  bool try_send_(HaHistorySensor *s, uint32_t now, bool seam);
  void handle_response_(HaHistorySensor *s, uint32_t anchor, uint32_t bucket_s, bool seam,
                        const api::ActionResponse &r);
  void fail_request_(HaHistorySensor *s, bool seam, ha_api_core::FailReason reason);
  void fail_after_error_(HaHistorySensor *s, bool seam);
  uint32_t window_start_for_(const HaHistorySensor *s, uint32_t now) const;
  std::string build_template_(const HaHistorySensor *s, uint32_t anchor) const;
  static void iso_utc_(uint32_t epoch, char *out, size_t len);

  ha_api_core::ActionClient client_;
  time::RealTimeClock *time_{nullptr};
  std::vector<HaHistorySensor *> sensors_;
  uint32_t retry_ms_{30000};
  // Diagnostics: silence is the failure mode of this transport, so every wait
  // state says what it is waiting for.
  bool clock_seen_{false};
  uint32_t clock_wait_logged_ms_{0};

  static constexpr uint32_t MAX_BACKOFF_MS = 600000;  // 10 min
  static constexpr uint8_t MAX_ATTEMPTS = 3;          // per connection
  static constexpr uint32_t SEAM_DELAY_MS = 420000;   // 7 min: HA's newest row lags up to 5
};

// Only when the config actually has a `button:` - the platform is optional, and
// including button.h unconditionally makes every button-less config fail to
// build.
#ifdef USE_BUTTON
/// A button that re-runs the backfill.
class HaHistoryReloadButton : public button::Button, public Component {
 public:
  void set_parent(HaHistory *hub) { this->hub_ = hub; }
  void dump_config() override;

 protected:
  void press_action() override {
    if (this->hub_ != nullptr)
      this->hub_->request_backfill();
  }
  HaHistory *hub_{nullptr};
};
#endif  // USE_BUTTON

}  // namespace ha_history
}  // namespace esphome
