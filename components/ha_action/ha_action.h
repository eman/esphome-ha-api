#pragma once

// One scheduled Home Assistant action call, and the bindings from its response
// to ESPHome entities.
//
// The gap this fills: ESPHome can already call an action and capture the
// response, but only as an automation action - you fire it yourself, it stores
// nothing, and the parsed document dies when your lambda returns. So the usual
// way to get a weather forecast onto a panel is to build template sensors in
// Home Assistant that flatten `weather.get_forecasts` into attributes and
// subscribe to those. That puts device-specific presentation logic in the hub
// and does not survive the second dashboard.
//
// Here the round trip is declarative: name an action, give it a schedule, and
// bind pieces of the response to entities with `path:`.
//
// Everything about the wire - the boot race, expiry, the frame guard, call_id
// allocation - is in ha_api_core/action_client.h.

#include "esphome/core/defines.h"

#ifdef USE_API

#include <string>
#include <vector>

#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif
#include "esphome/components/json/json_util.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"

#include "esphome/components/ha_api_core/action_client.h"
#include "path.h"

namespace esphome {
namespace ha_action {

/// A binding from a path into the response to something that publishes.
/// Implemented by the sensor / text_sensor / binary_sensor platforms.
class Target {
 public:
  virtual ~Target() = default;

  // Called by codegen, once per compiled token (see path.py).
  void add_path_key(const char *key) { this->path_.push_back(PathToken{key, 0}); }
  void add_path_index(int32_t index) { this->path_.push_back(PathToken{nullptr, index}); }

  /// `root` is the action's response. Resolving to nothing is normal - a
  /// forecast is shorter tomorrow than today - so implementations publish their
  /// "unknown" rather than logging an error.
  virtual void publish_from(JsonVariantConst root) = 0;

  /// For dump_config, so a mis-typed path is visible without a reply.
  std::string path_str() const;

 protected:
  Path path_;
};

class HaAction : public Component {
 public:
  HaAction();

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  void set_name(const char *name) { this->name_ = name; }
  void set_action(const char *action) { this->action_ = action; }
  void add_data(const char *key, TemplatableValue<std::string> value) {
    this->data_.emplace_back(key, std::move(value));
  }
  void add_data_template(const char *key, TemplatableValue<std::string> value) {
    this->data_template_.emplace_back(key, std::move(value));
  }
  void add_variable(const char *key, TemplatableValue<std::string> value) {
    this->variables_.emplace_back(key, std::move(value));
  }
  void set_response_template(TemplatableValue<std::string> tmpl) {
    this->response_template_ = std::move(tmpl);
    this->has_template_ = true;
  }
  void set_update_interval(uint32_t ms) { this->update_interval_ms_ = ms; }
  /// `update_interval: never` - the request runs only when something asks,
  /// via the refresh button or another request's on_response.
  void set_manual(bool manual) { this->manual_ = manual; }
  void set_retry_interval(uint32_t ms) { this->retry_ms_ = ms; }
  void set_timeout(uint32_t ms) { this->client_.set_deadline(ms); }
  void set_retain(bool retain) { this->retain_ = retain; }
  void add_target(Target *target) { this->targets_.push_back(target); }

  Trigger<JsonVariantConst> *get_response_trigger() { return &this->response_trigger_; }
  Trigger<std::string> *get_error_trigger() { return &this->error_trigger_; }

  /// Ask again now, resetting any backoff. Bound to the refresh button; the
  /// case it exists for is "the user just enabled actions in Home Assistant".
  void refresh();

  /// True once a reply has been merged at least once.
  bool has_response() const { return this->have_response_; }

  /// The retained response, for renderers that want to walk a whole array.
  /// Null unless `retain: true` - without it the parsed document lives only for
  /// the duration of the reply callback.
  JsonVariantConst response() const { return this->retained_.as<JsonVariantConst>(); }

 protected:
  bool try_send_();
  void handle_reply_(const api::ActionResponse &r);
  void fail_(ha_api_core::FailReason reason);
  /// Home Assistant reached the action and it failed - a different thing
  /// from the transport failing, and not evidence that actions are blocked.
  void fail_after_error_();
  void schedule_retry_(const char *why);
  void on_api_change_(bool connected, uint32_t outage_ms);
  /// Unwraps Home Assistant's {"response": ...} envelope, which is present
  /// whether or not a response_template was used.
  JsonVariantConst unwrap_(JsonObjectConst root, JsonDocument &scratch) const;

  ha_api_core::ActionClient client_;
  const char *name_{""};
  const char *action_{nullptr};
  std::vector<std::pair<const char *, TemplatableValue<std::string>>> data_;
  std::vector<std::pair<const char *, TemplatableValue<std::string>>> data_template_;
  std::vector<std::pair<const char *, TemplatableValue<std::string>>> variables_;
  TemplatableValue<std::string> response_template_;
  std::vector<Target *> targets_;
  Trigger<JsonVariantConst> response_trigger_;
  Trigger<std::string> error_trigger_;
  // `retain: true` keeps the parsed response alive between refreshes, so it is
  // a permanent allocation - worth putting in PSRAM on a board that has it,
  // where a whole day's forecast is otherwise a few KB of internal heap.
#ifdef USE_PSRAM
  json::SpiRamAllocator allocator_;
  JsonDocument retained_{&allocator_};
#else
  JsonDocument retained_;
#endif

  uint32_t update_interval_ms_{0};  // 0 = once per connection
  uint32_t retry_ms_{30000};
  uint32_t backoff_ms_{0};
  uint32_t next_due_ms_{0};
  uint8_t attempts_{0};
  uint8_t timeouts_{0};
  bool has_template_{false};
  bool retain_{false};
  bool have_response_{false};
  bool manual_{false};
  bool due_{false};

  static constexpr uint32_t MAX_BACKOFF_MS = 600000;  // 10 min
  static constexpr uint8_t MAX_ATTEMPTS = 3;          // per connection
};

// Only when the config actually has a `button:`. The platform is optional, and
// including button.h unconditionally makes every button-less config fail to
// build - which is exactly the bug this component found in ha_history.
#ifdef USE_BUTTON
/// A button that re-runs one request.
class HaActionRefreshButton : public button::Button, public Component {
 public:
  void set_parent(HaAction *parent) { this->parent_ = parent; }
  void dump_config() override;

 protected:
  void press_action() override {
    if (this->parent_ != nullptr)
      this->parent_->refresh();
  }
  HaAction *parent_{nullptr};
};
#endif  // USE_BUTTON

}  // namespace ha_action
}  // namespace esphome

#endif  // USE_API
