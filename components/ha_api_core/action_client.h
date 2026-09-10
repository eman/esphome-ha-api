#pragma once

// The Home Assistant action transport.
//
// This component has no YAML surface of its own. It exists so that `ha_action`
// and `ha_history` can share one implementation of the wire without either
// depending on the other: ESPHome copies whole component directories, so a
// shared file living inside one of them would drag that component's entire
// implementation into every build of the other.
//
// Home Assistant already holds an encrypted native-API connection to every
// adopted device, and that connection can carry an action call with a response.
// ESPHome exposes it as the `homeassistant.action` automation action, which is
// fine for a fire-and-forget call from a button press but has four properties
// that make it unusable for scheduled, unattended use. This class exists to fix
// all four, once, for whatever sits on top:
//
//  * `HomeAssistantServiceCallAction::play()` registers its response callback
//    BEFORE sending, and `APIServer::send_homeassistant_action()` returns void.
//    Home Assistant subscribes to actions shortly AFTER authenticating, so a
//    call fired at boot is dropped - and its callback is already registered.
//    Here, the per-client `APIConnection::send_homeassistant_action()` returns
//    false in exactly that window, so nothing is registered until a send was
//    accepted, and `send()` returning false simply means "try again shortly".
//
//  * Registered callbacks are erased only by a matching reply. There is no
//    timeout and no removal API, so an unanswered call leaks its callback
//    forever. Here the callback captures a weak_ptr and the client keeps its own
//    deadline, so a late reply is ignored and an absent one is bounded.
//
//  * The built-in's call_id counter is a function-local static PER TEMPLATE
//    INSTANTIATION, so two `homeassistant.action` blocks both start at 1 and
//    cross-deliver each other's responses. Ours is one counter shared by every
//    ActionClient in the binary (see call_id_next_), seeded far away from the
//    built-in's.
//
//  * The API frame cap is 32 KiB and exceeding it DROPS THE CONNECTION, after
//    which Home Assistant reconnects and a scheduled caller would ask again -
//    a flap loop that takes every entity on the device down with it. A
//    connection lost within TOO_LARGE_WINDOW_MS of a send is reported as
//    FailReason::TOO_LARGE so the caller can back off hard instead.
//
// Retry policy deliberately does NOT live here. Both callers track attempts per
// target - per sensor in ha_history, per request in ha_action - because the
// backoff has to be per-target to be useful. This class owns the wire.

#include "esphome/core/defines.h"

#ifdef USE_API

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "esphome/components/api/api_pb2.h"
#include "esphome/components/api/api_server.h"
#include "esphome/components/api/homeassistant_service.h"

#if !defined(USE_API_HOMEASSISTANT_ACTION_RESPONSES_JSON)
#error "This component needs Home Assistant action responses; its Python sets the defines - is the hub loaded?"
#endif

namespace esphome {
namespace ha_api_core {

/// One outgoing action call.
///
/// Every string referenced here must outlive the send(): the protobuf message
/// holds StringRefs into `action`, the `data` values and `response_template`,
/// and they are read during serialisation inside send().
struct ActionRequest {
  /// "weather.get_forecasts", "recorder.get_statistics", ...
  const char *action{nullptr};
  /// Sent as plain strings. Home Assistant's service schemas coerce with
  /// `cv.ensure_list`, so a single value needs no list wrapper.
  std::vector<std::pair<const char *, std::string>> data;
  /// Rendered as Jinja by Home Assistant, then literal_eval'd - which is the
  /// only way to pass anything that is not a string. `["a", "b"]` here arrives
  /// as a real two-element list, where the same text in `data` would arrive as
  /// one string and be wrapped into a one-element list by ensure_list.
  std::vector<std::pair<const char *, std::string>> data_template;
  /// Variables in scope for `data_template`.
  std::vector<std::pair<const char *, std::string>> variables;
  /// Jinja that Home Assistant renders SERVER-SIDE before the reply crosses the
  /// wire. Empty means "send the raw response". Built at runtime by both
  /// callers, which is why this is a std::string and not a const char *.
  std::string response_template;
};

/// Why a request ended without a usable reply. Home Assistant returning an
/// error is not here - that arrives as a normal reply with is_success() false.
enum class FailReason : uint8_t {
  /// The deadline passed in silence. Home Assistant sends nothing at all when
  /// "Allow the device to perform Home Assistant actions" is off, so this is
  /// the only symptom that failure mode has.
  TIMEOUT,
  /// The API connection went away while the request was in flight.
  DISCONNECTED,
  /// The connection died within TOO_LARGE_WINDOW_MS of the send, which is the
  /// signature of a reply that exceeded the 32 KiB API frame.
  TOO_LARGE,
};

const char *to_string(FailReason reason);

class ActionClient {
 public:
  using ReplyCb = std::function<void(const api::ActionResponse &)>;
  using FailCb = std::function<void(FailReason)>;
  /// (connected, milliseconds the API was down - only meaningful on connect).
  using ApiChangeCb = std::function<void(bool, uint32_t)>;

  /// `tag` is the owning component's log tag; every line this class emits uses
  /// it, so the logs read as one component rather than two.
  explicit ActionClient(const char *tag) : tag_(tag) {}

  /// Must be called from the owner's loop(): detects API connect/disconnect and
  /// expires the in-flight request.
  void loop();

  /// Sends to the first API client that has subscribed to Home Assistant
  /// actions.
  ///
  /// Returns false when no connected client has subscribed yet - nothing was
  /// sent, nothing was registered, and the caller should retry in a second or
  /// so. That is the normal state for the first moments after boot, not an
  /// error; the client warns on its own if it lasts.
  ///
  /// One request at a time: check busy() first. Serialising them bounds the
  /// transient JsonDocument and the receive frame to a single payload, which is
  /// what keeps this affordable on a device without PSRAM.
  bool send(const ActionRequest &req, const char *what, ReplyCb on_reply, FailCb on_fail);

  /// Abandons any in-flight request without invoking its failure callback. A
  /// reply that arrives afterwards is ignored.
  void cancel();

  bool busy() const { return this->pending_ != nullptr; }
  bool connected() const { return this->connected_; }

  void set_deadline(uint32_t ms) { this->deadline_ms_ = ms; }
  void set_on_api_change(ApiChangeCb cb) { this->on_api_change_ = std::move(cb); }

  static constexpr uint32_t DEFAULT_DEADLINE_MS = 20000;
  static constexpr uint32_t TOO_LARGE_WINDOW_MS = 2000;
  /// How long an unsubscribed connection is tolerated before saying so. Home
  /// Assistant subscribes within a second or two of authenticating.
  static constexpr uint32_t PROBE_WARN_MS = 10000;

 protected:
  struct Pending {
    uint32_t call_id;
    uint32_t sent_ms;
    std::string what;
    ReplyCb on_reply;
    FailCb on_fail;
  };

  void fail_pending_(FailReason reason);
  void note_probe_failure_(uint32_t now_ms);

  const char *tag_;
  std::shared_ptr<Pending> pending_;
  ApiChangeCb on_api_change_;
  uint32_t deadline_ms_{DEFAULT_DEADLINE_MS};
  uint32_t disconnected_ms_{0};
  uint32_t probe_fail_since_ms_{0};
  bool probe_warned_{false};
  bool connected_{false};

  /// Shared by every ActionClient in the binary, so ha_action and ha_history
  /// cannot hand Home Assistant the same call_id. Non-zero because Home
  /// Assistant dispatches on `if call_id`, and far from the built-in action's
  /// per-instantiation counters, which start at 1.
  static uint32_t call_id_next_;
};

}  // namespace ha_api_core
}  // namespace esphome

#endif  // USE_API
