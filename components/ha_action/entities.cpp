#include "entities.h"

#ifdef USE_API

#include "esphome/core/log.h"

namespace esphome {
namespace ha_action {

static const char *const TAG = "ha_action";

#ifdef USE_SENSOR
void HaActionSensor::dump_config() {
  LOG_SENSOR("", "Home Assistant action sensor", this);
  ESP_LOGCONFIG(TAG, "  Path: %s", this->path_str().c_str());
}
#endif

#ifdef USE_TEXT_SENSOR
void HaActionTextSensor::dump_config() {
  LOG_TEXT_SENSOR("", "Home Assistant action text sensor", this);
  ESP_LOGCONFIG(TAG, "  Path: %s", this->path_str().c_str());
}
#endif

#ifdef USE_BINARY_SENSOR
void HaActionBinarySensor::dump_config() {
  LOG_BINARY_SENSOR("", "Home Assistant action binary sensor", this);
  ESP_LOGCONFIG(TAG, "  Path: %s", this->path_str().c_str());
  if (this->has_threshold_)
    ESP_LOGCONFIG(TAG, "  True above: %.3f", this->threshold_);
}
#endif

}  // namespace ha_action
}  // namespace esphome

#endif  // USE_API
