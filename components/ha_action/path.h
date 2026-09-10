#pragma once

// Selecting one value out of an action response.
//
// Home Assistant responses are keyed by ENTITY ID, and entity ids contain dots:
// `weather.get_forecasts` returns
//
//     {"weather.home": {"forecast": [{"datetime": ..., "temperature": 14.2}]}}
//
// so a plain dotted path is ambiguous on the very first segment of the most
// common response shape in the domain. The syntax therefore has a bracket form
// that takes a quoted key:
//
//     ["weather.home"].forecast[0].temperature
//
// The grammar is parsed in Python at config time (see compile_path in
// __init__.py) and emitted as a token list, so `esphome config` reports a bad
// path with a caret under the offending character and this header only ever
// walks tokens. There is deliberately no parser here: one grammar, in one
// place, cannot drift out of agreement with itself.

#include "esphome/core/defines.h"

#ifdef USE_API

#include <cstdint>
#include <vector>

#include "esphome/components/json/json_util.h"

namespace esphome {
namespace ha_action {

/// One step of a path: an object key, or an array index.
struct PathToken {
  /// nullptr means "this token is an index". Keys point at flash literals
  /// emitted by codegen, so there is nothing to free and nothing to copy.
  const char *key;
  int32_t index;

  bool is_index() const { return this->key == nullptr; }
};

using Path = std::vector<PathToken>;

/// Walks `path` from `root`.
///
/// Returns a null variant when any step does not exist: a missing key, an index
/// past the end, or a type that cannot be subscripted the way the path asks. A
/// forecast that is 12 entries long today and 10 tomorrow is a normal thing for
/// a path to run off the end of, so callers treat null as "no value right now"
/// rather than as an error.
inline JsonVariantConst resolve_path(JsonVariantConst root, const Path &path) {
  JsonVariantConst v = root;
  for (const auto &t : path) {
    if (t.is_index()) {
      JsonArrayConst arr = v.as<JsonArrayConst>();
      if (arr.isNull())
        return JsonVariantConst();
      // Negative indices count from the end, so [-1] is "the most recent
      // entry" without knowing how many there are.
      int32_t i = t.index;
      if (i < 0)
        i += (int32_t) arr.size();
      if (i < 0 || (size_t) i >= arr.size())
        return JsonVariantConst();
      v = arr[(size_t) i];
    } else {
      JsonObjectConst obj = v.as<JsonObjectConst>();
      if (obj.isNull())
        return JsonVariantConst();
      v = obj[t.key];
    }
    if (v.isNull())
      return JsonVariantConst();
  }
  return v;
}

}  // namespace ha_action
}  // namespace esphome

#endif  // USE_API
