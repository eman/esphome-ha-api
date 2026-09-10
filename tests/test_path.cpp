// Host tests for resolve_path(). The payloads below are the real shapes Home
// Assistant returns, because the reason this grammar exists at all is that
// those shapes are keyed by entity id.

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

#include "../components/ha_action/path.h"

using esphome::ha_action::Path;
using esphome::ha_action::PathToken;
using esphome::ha_action::resolve_path;

static PathToken key(const char *k) { return PathToken{k, 0}; }
static PathToken idx(int32_t i) { return PathToken{nullptr, i}; }

// weather.get_forecasts, trimmed. Note the top-level key: an entity id.
static const char *FORECAST = R"({
  "weather.home": {
    "forecast": [
      {"datetime": "2026-09-10T17:00:00+00:00", "condition": "sunny",
       "temperature": 21.4, "precipitation": 0.0, "is_daytime": true},
      {"datetime": "2026-09-10T18:00:00+00:00", "condition": "cloudy",
       "temperature": 19.8, "precipitation": 0.6, "is_daytime": true},
      {"datetime": "2026-09-10T19:00:00+00:00", "condition": "rainy",
       "temperature": 17.1, "precipitation": 2.4, "is_daytime": false}
    ]
  }
})";

// tariffkit.get_rates, trimmed.
static const char *RATES = R"({
  "points": [
    {"start": "2026-09-10T00:00:00-07:00", "import": 0.081, "export": 0.043},
    {"start": "2026-09-10T17:00:00-07:00", "import": 0.612, "export": 1.164}
  ],
  "quality": {"complete": true, "warnings": []}
})";

static int failures = 0;

#define CHECK(cond, what) \
  do { \
    if (!(cond)) { \
      std::printf("  FAIL: %s\n", what); \
      failures++; \
    } \
  } while (0)

static void test_forecast() {
  JsonDocument doc;
  assert(deserializeJson(doc, FORECAST) == DeserializationError::Ok);
  JsonVariantConst root = doc.as<JsonVariantConst>();

  // ["weather.home"].forecast[0].temperature
  Path p{key("weather.home"), key("forecast"), idx(0), key("temperature")};
  CHECK(std::fabs(resolve_path(root, p).as<float>() - 21.4f) < 1e-4, "forecast[0].temperature");

  // A dotted key MUST go through the bracket form; the whole reason for it.
  Path split{key("weather"), key("home")};
  CHECK(resolve_path(root, split).isNull(), "'weather'.'home' does not resolve the entity-id key");

  // Strings and bools come back intact.
  Path cond{key("weather.home"), key("forecast"), idx(1), key("condition")};
  CHECK(std::string(resolve_path(root, cond).as<const char *>()) == "cloudy", "forecast[1].condition");
  Path day{key("weather.home"), key("forecast"), idx(2), key("is_daytime")};
  CHECK(resolve_path(root, day).as<bool>() == false, "forecast[2].is_daytime");

  // Negative indices count from the end - "the last entry" without knowing how
  // many a forecast has today.
  Path last{key("weather.home"), key("forecast"), idx(-1), key("temperature")};
  CHECK(std::fabs(resolve_path(root, last).as<float>() - 17.1f) < 1e-4, "forecast[-1].temperature");
  Path first_from_end{key("weather.home"), key("forecast"), idx(-3), key("condition")};
  CHECK(std::string(resolve_path(root, first_from_end).as<const char *>()) == "sunny", "forecast[-3]");

  // Running off either end is normal, not an error: a forecast is 12 entries
  // long today and 10 tomorrow.
  Path past_end{key("weather.home"), key("forecast"), idx(99), key("temperature")};
  CHECK(resolve_path(root, past_end).isNull(), "index past the end is null");
  Path before_start{key("weather.home"), key("forecast"), idx(-99)};
  CHECK(resolve_path(root, before_start).isNull(), "index before the start is null");

  // Missing keys, at every depth.
  Path missing_top{key("weather.away"), key("forecast"), idx(0)};
  CHECK(resolve_path(root, missing_top).isNull(), "missing top-level key is null");
  Path missing_leaf{key("weather.home"), key("forecast"), idx(0), key("humidity")};
  CHECK(resolve_path(root, missing_leaf).isNull(), "missing leaf key is null");

  // Type mismatches: indexing an object, keying an array, descending past a
  // scalar. None may read out of bounds or crash.
  Path index_an_object{key("weather.home"), idx(0)};
  CHECK(resolve_path(root, index_an_object).isNull(), "indexing an object is null");
  Path key_an_array{key("weather.home"), key("forecast"), key("temperature")};
  CHECK(resolve_path(root, key_an_array).isNull(), "keying an array is null");
  Path past_scalar{key("weather.home"), key("forecast"), idx(0), key("temperature"), key("nope")};
  CHECK(resolve_path(root, past_scalar).isNull(), "descending past a scalar is null");

  // An empty path is the document itself.
  Path empty{};
  CHECK(resolve_path(root, empty).as<JsonObjectConst>().size() == 1, "empty path is the root");
}

static void test_rates() {
  JsonDocument doc;
  assert(deserializeJson(doc, RATES) == DeserializationError::Ok);
  JsonVariantConst root = doc.as<JsonVariantConst>();

  Path peak{key("points"), idx(1), key("export")};
  CHECK(std::fabs(resolve_path(root, peak).as<float>() - 1.164f) < 1e-4, "points[1].export");

  Path complete{key("quality"), key("complete")};
  CHECK(resolve_path(root, complete).as<bool>(), "quality.complete");

  // An empty array is a real value, not a missing one - but it has no [0].
  Path warnings{key("quality"), key("warnings")};
  CHECK(resolve_path(root, warnings).as<JsonArrayConst>().size() == 0, "quality.warnings is an empty array");
  Path first_warning{key("quality"), key("warnings"), idx(0)};
  CHECK(resolve_path(root, first_warning).isNull(), "quality.warnings[0] is null");
}

static void test_null_and_zero() {
  // A JSON null and a genuine 0 must not be confused: precipitation of 0.0 is
  // data, and callers publish NaN only for the null.
  JsonDocument doc;
  assert(deserializeJson(doc, R"({"a": {"zero": 0, "null": null, "false": false, "empty": ""}})") ==
         DeserializationError::Ok);
  JsonVariantConst root = doc.as<JsonVariantConst>();

  Path zero{key("a"), key("zero")};
  CHECK(!resolve_path(root, zero).isNull(), "0 is not null");
  CHECK(resolve_path(root, zero).as<float>() == 0.0f, "0 reads as 0");
  Path fals{key("a"), key("false")};
  CHECK(!resolve_path(root, fals).isNull(), "false is not null");
  Path empty_str{key("a"), key("empty")};
  CHECK(!resolve_path(root, empty_str).isNull(), "\"\" is not null");
  Path nul{key("a"), key("null")};
  CHECK(resolve_path(root, nul).isNull(), "JSON null is null");
}

int main() {
  test_forecast();
  test_rates();
  test_null_and_zero();
  if (failures != 0) {
    std::printf("path.h: %d test(s) failed\n", failures);
    return 1;
  }
  std::printf("path.h: all tests passed\n");
  return 0;
}
