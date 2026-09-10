#pragma once

// A Home Assistant sensor that arrives with its recent past already loaded.
//
// Behaves exactly like ESPHome's built-in `homeassistant` sensor - the same
// live subscription over the native API, the same NAN on a non-numeric state -
// and additionally keeps a buffer of one value per bucket (5 minutes or an
// hour) over a window (a rolling duration, or today so far). The hub fills the
// buffer from Home Assistant's recorder statistics at startup; the live pushes
// extend it from then on.

#include "esphome/components/sensor/sensor.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include "series.h"

namespace esphome {
namespace ha_history {

class HaHistory;

enum class WindowKind : uint8_t { ROLLING, TODAY };

/// Where a sensor is in its backfill lifecycle. Driven by the hub.
enum class LoadState : uint8_t {
  IDLE,      // wants a request
  PENDING,   // request in flight
  LOADED,    // buffer holds HA's rows; a seam follow-up may still be due
  GIVEN_UP,  // too many unanswered attempts on this connection
};

class HaHistorySensor : public sensor::Sensor, public Component {
 public:
  // The two setters `esphome.components.homeassistant.setup_home_assistant_entity`
  // calls, so its schema and helper are reused unchanged.
  void set_entity_id(const char *entity_id) { this->entity_id_ = entity_id; }
  void set_attribute(const char *attribute) { this->attribute_ = attribute; }

  void set_parent(HaHistory *hub) { this->hub_ = hub; }
  void set_window(WindowKind kind, uint32_t seconds) {
    this->window_kind_ = kind;
    this->window_s_ = seconds;
  }
  void set_bucket(uint32_t seconds) { this->bucket_s_ = seconds; }
  void set_statistic(Statistic s) { this->statistic_ = s; }
  void set_capacity(uint16_t points) { this->capacity_ = points; }

  void setup() override;
  void dump_config() override;
  // Same priority as the built-in platform: the API server exists by then.
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  // ---- for renderers -----------------------------------------------------
  bool history_loaded() const { return this->load_state_ == LoadState::LOADED; }
  size_t history_size() const { return this->buffer_.size(); }
  /// Copies out as parallel arrays, oldest first. Returns the count.
  size_t history_copy(uint32_t *starts, float *values, size_t cap) const {
    return this->buffer_.copy(starts, values, cap);
  }
  float history_min() const { return this->buffer_.min_value(); }
  float history_max() const { return this->buffer_.max_value(); }
  uint32_t bucket_seconds() const { return this->bucket_s_; }
  /// Start of the window as the renderer should draw it: local midnight for
  /// `today`, `now - window` for a rolling window. Epoch UTC; 0 until the clock
  /// is valid.
  uint32_t window_start() const { return this->window_start_; }
  uint32_t window_seconds() const { return this->window_s_; }
  const SeriesBuffer &history() const { return this->buffer_; }

  Trigger<> *get_loaded_trigger() { return &this->loaded_trigger_; }
  Trigger<> *get_update_trigger() { return &this->update_trigger_; }

  const char *get_entity_id() const { return this->entity_id_; }
  Statistic get_statistic() const { return this->statistic_; }
  WindowKind get_window_kind() const { return this->window_kind_; }

 protected:
  friend class HaHistory;

  /// Live value arrived from Home Assistant.
  void on_state_(float value);

  const char *entity_id_{nullptr};
  const char *attribute_{nullptr};
  HaHistory *hub_{nullptr};

  WindowKind window_kind_{WindowKind::ROLLING};
  uint32_t window_s_{86400};
  uint32_t bucket_s_{300};
  Statistic statistic_{Statistic::MEAN};
  uint16_t capacity_{300};

  RAMAllocator<Point> allocator_{};
  SeriesBuffer buffer_;
  BucketAccumulator acc_;
  /// The most recent live value, kept so the first bucket after boot can be
  /// seeded even when the clock became valid after the value arrived.
  float last_seen_{NAN};
  uint32_t window_start_{0};
  int32_t last_local_day_{-1};  // for the midnight clear with window: today

  // Backfill bookkeeping, owned by the hub.
  LoadState load_state_{LoadState::IDLE};
  uint8_t attempts_{0};   // unanswered attempts on this connection
  uint8_t timeouts_{0};   // consecutive silent timeouts (the "actions disabled" signature)
  uint32_t next_try_ms_{0};
  uint32_t backoff_ms_{0};
  uint32_t loaded_ms_{0};
  uint32_t boot_bucket_start_{0};  // the bucket that was in progress at boot
  bool seam_done_{false};
  bool seam_request_{false};       // the current request is the narrow seam repair
  uint32_t last_loaded_points_{0};

  Trigger<> loaded_trigger_;
  Trigger<> update_trigger_;
};

}  // namespace ha_history
}  // namespace esphome
