#include "ha_history_sensor.h"

#include "esphome/components/api/api_server.h"
#include "esphome/core/log.h"
#include "esphome/core/string_ref.h"

#include "ha_history.h"

namespace esphome {
namespace ha_history {

static const char *const TAG = "ha_history.sensor";

void HaHistorySensor::setup() {
  // The buffer lives in PSRAM when there is any; internal RAM otherwise. Sized
  // once from the config-time capacity, never grown.
  Point *storage = this->allocator_.allocate(this->capacity_);
  if (storage == nullptr) {
    ESP_LOGE(TAG, "'%s': could not allocate %u history points", this->entity_id_, this->capacity_);
    this->mark_failed();
    return;
  }
  this->buffer_.attach(storage, this->capacity_);

  // Identical to the built-in `homeassistant` sensor's subscription
  // (homeassistant/sensor/homeassistant_sensor.cpp), plus the feed into the
  // bucket accumulator.
  api::global_api_server->subscribe_home_assistant_state(this->entity_id_, this->attribute_, [this](StringRef state) {
    auto val = parse_number<float>(state.c_str());
    if (!val.has_value()) {
      // `unavailable` / `unknown`. The sensor reports NAN as the built-in one
      // does; the buffer holds the previous value, as Home Assistant's own
      // statistics do.
      ESP_LOGW(TAG, "'%s': Can't convert '%s' to number!", this->entity_id_, state.c_str());
      this->publish_state(NAN);
      return;
    }
    ESP_LOGV(TAG, "'%s': Got state %.2f", this->entity_id_, *val);
    this->publish_state(*val);
    this->on_state_(*val);
  });
}

void HaHistorySensor::on_state_(float value) {
  this->last_seen_ = value;
  if (this->hub_ == nullptr)
    return;
  const uint32_t now = this->hub_->now_utc();
  if (now == 0)
    return;  // no clock yet; last_seen_ will seed the first bucket
  this->acc_.add(value, now);
}

void HaHistorySensor::dump_config() {
  LOG_SENSOR("", "Home Assistant history sensor", this);
  ESP_LOGCONFIG(TAG, "  Entity ID: '%s'", this->entity_id_);
  ESP_LOGCONFIG(TAG, "  Window: %s (%us)", this->window_kind_ == WindowKind::TODAY ? "today" : "rolling",
                (unsigned) this->window_s_);
  ESP_LOGCONFIG(TAG, "  Bucket: %us, statistic: %s, capacity: %u points", (unsigned) this->bucket_s_,
                to_string(this->statistic_), (unsigned) this->capacity_);
  ESP_LOGCONFIG(TAG, "  History: %s, %u points held, last load %u points",
                this->load_state_ == LoadState::LOADED     ? "loaded"
                : this->load_state_ == LoadState::PENDING  ? "pending"
                : this->load_state_ == LoadState::GIVEN_UP ? "given up"
                                                           : "not loaded",
                (unsigned) this->buffer_.size(), (unsigned) this->last_loaded_points_);
}

}  // namespace ha_history
}  // namespace esphome
