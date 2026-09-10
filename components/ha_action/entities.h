#pragma once

// The three entity platforms: a compiled path, resolved against each response
// and published. Deliberately thin - the interesting parts are the transport
// (action_client.h) and the grammar (path.h).

#include "esphome/core/defines.h"

#ifdef USE_API

#include <cmath>

#include "ha_action.h"
#include "path.h"

#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif

namespace esphome {
namespace ha_action {

#ifdef USE_SENSOR
class HaActionSensor : public sensor::Sensor, public Target, public Component {
 public:
  void dump_config() override;
  void publish_from(JsonVariantConst root) override {
    JsonVariantConst v = resolve_path(root, this->path_);
    // NaN is ESPHome's "unknown", and it is the right answer here: a forecast
    // 12 entries long today and 10 tomorrow makes running off the end normal.
    // Note that a JSON null gives NaN while a genuine 0 does not - 0.0 mm of
    // precipitation is data.
    this->publish_state(v.isNull() ? NAN : v.as<float>());
  }
};
#endif

#ifdef USE_TEXT_SENSOR
class HaActionTextSensor : public text_sensor::TextSensor, public Target, public Component {
 public:
  void dump_config() override;
  void publish_from(JsonVariantConst root) override {
    JsonVariantConst v = resolve_path(root, this->path_);
    if (v.isNull()) {
      this->publish_state("");
      return;
    }
    if (v.is<const char *>()) {
      this->publish_state(v.as<const char *>());
      return;
    }
    // Numbers, booleans, and whole objects or arrays: serialise rather than
    // publish nothing. Pointing a text sensor at a number is a reasonable
    // thing to do, and seeing `{"a":1}` beats seeing "".
    std::string out;
    serializeJson(v, out);
    this->publish_state(out);
  }
};
#endif

#ifdef USE_BINARY_SENSOR
class HaActionBinarySensor : public binary_sensor::BinarySensor, public Target, public Component {
 public:
  void dump_config() override;
  /// Above this, the value is true. Unset means the JSON is already a boolean.
  void set_threshold(float threshold) {
    this->threshold_ = threshold;
    this->has_threshold_ = true;
  }
  void publish_from(JsonVariantConst root) override {
    JsonVariantConst v = resolve_path(root, this->path_);
    // A binary sensor has no "unknown" to publish, so an absent value leaves
    // the last known state alone rather than inventing `false`.
    if (v.isNull())
      return;
    this->publish_state(this->has_threshold_ ? v.as<float>() > this->threshold_ : v.as<bool>());
  }

 protected:
  float threshold_{0.0f};
  bool has_threshold_{false};
};
#endif

}  // namespace ha_action
}  // namespace esphome

#endif  // USE_API
