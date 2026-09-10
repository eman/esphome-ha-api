#pragma once

// The hub: turns live Home Assistant pushes into buckets, and fills each
// sensor's buffer with Home Assistant's recorder statistics at startup.
//
// The backfill rides the native API connection Home Assistant already holds
// to the device: the hub sends a `recorder.get_statistics` action with a
// `response_template`, Home Assistant renders that Jinja server-side into a
// compact list of (bucket index, value) pairs, and the reply arrives through
// APIServer::register_action_response_callback. No HTTP client, no TLS, no
// long-lived token.
//
// Three facts about that path shape everything below (see README):
//  * The API frame limit is 32 KiB and exceeding it DROPS THE CONNECTION, after
//    which Home Assistant reconnects and the hub would ask again - a flap loop
//    that takes every entity on the device down. So the payload is bounded at
//    config time, and a connection lost right after a request is treated as
//    "reply too large" and backed off hard.
//  * Registered callbacks have no timeout and no removal API. The hub keeps its
//    own deadline and ignores late replies through a weak_ptr.
//  * Home Assistant subscribes to actions shortly AFTER authenticating, and an
//    action sent before that is silently dropped. APIConnection::
//    send_homeassistant_action() returns false in exactly that window, so the
//    hub asks one client at a time and only registers a callback once a send
//    was accepted.

#include <memory>
#include <string>
#include <vector>

#include "esphome/components/api/api_pb2.h"
#include "esphome/components/api/api_server.h"
#include "esphome/components/api/homeassistant_service.h"
#include "esphome/components/button/button.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/core/component.h"

#include "ha_history_sensor.h"

#if !defined(USE_API_HOMEASSISTANT_ACTION_RESPONSES_JSON)
#error "ha_history needs Home Assistant action responses; the component's Python sets the defines - is the hub loaded?"
#endif

namespace esphome {
namespace ha_history {

class HaHistory : public Component {
 public:
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
  struct Pending {
    HaHistorySensor *sensor;
    uint32_t call_id;
    uint32_t sent_ms;
    uint32_t anchor;
    uint32_t bucket_s;
    bool seam;
  };

  void tick_buckets_(uint32_t now);
  void tick_backfill_(uint32_t now_ms, uint32_t now);
  bool try_send_(HaHistorySensor *s, uint32_t now, bool seam);
  void handle_response_(const std::shared_ptr<Pending> &p, const api::ActionResponse &r);
  void fail_pending_(const char *why, bool too_large);
  void fail_after_error_(HaHistorySensor *s, bool seam);
  uint32_t window_start_for_(const HaHistorySensor *s, uint32_t now) const;
  std::string build_template_(const HaHistorySensor *s, uint32_t anchor) const;
  static void iso_utc_(uint32_t epoch, char *out, size_t len);

  time::RealTimeClock *time_{nullptr};
  std::vector<HaHistorySensor *> sensors_;
  uint32_t retry_ms_{30000};
  // Non-zero (HA dispatches on `if call_id`), and far from the per-instantiation
  // counters YAML actions use, which start at 1.
  uint32_t call_id_next_{0x48480000};
  std::shared_ptr<Pending> pending_;
  bool api_was_connected_{false};
  uint32_t disconnected_ms_{0};
  // Diagnostics: silence is the failure mode of this transport, so every wait
  // state says what it is waiting for.
  bool clock_seen_{false};
  uint32_t clock_wait_logged_ms_{0};
  uint32_t probe_fail_since_ms_{0};
  bool probe_warned_{false};

  static constexpr uint32_t DEADLINE_MS = 20000;
  static constexpr uint32_t MAX_BACKOFF_MS = 600000;   // 10 min
  static constexpr uint8_t MAX_ATTEMPTS = 3;           // per connection
  static constexpr uint32_t SEAM_DELAY_MS = 420000;    // 7 min: HA's newest row lags up to 5
  static constexpr uint32_t TOO_LARGE_WINDOW_MS = 2000;
};

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

}  // namespace ha_history
}  // namespace esphome
