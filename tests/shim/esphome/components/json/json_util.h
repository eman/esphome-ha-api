#pragma once
// Host-test shim. ESPHome's real json_util.h pulls in ArduinoJson plus its own
// helpers and PSRAM allocator; resolve_path uses none of that, only the types.
#include <ArduinoJson.h>
