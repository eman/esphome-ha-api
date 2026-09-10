"""ha_history — Home Assistant sensors that arrive with their recent history.

The hub holds the defaults and the backfill machinery. It is auto-loaded by the
`ha_history` sensor platform, so `ha_history:` only needs to appear in YAML to
change a default.
"""

import esphome.codegen as cg
from esphome.components import time as time_
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TIME_ID
from esphome.core import TimePeriod

CODEOWNERS = ["@emansl"]
DEPENDENCIES = ["api", "time"]
# api's own AUTO_LOAD only adds json when a YAML `homeassistant.action` sets
# capture_response; we turn action responses on ourselves, so we add it.
AUTO_LOAD = ["json"]

CONF_WINDOW = "window"
CONF_BUCKET = "bucket"
CONF_RETRY_INTERVAL = "retry_interval"
CONF_HA_HISTORY_ID = "ha_history_id"

ha_history_ns = cg.esphome_ns.namespace("ha_history")
HaHistory = ha_history_ns.class_("HaHistory", cg.Component)

# Bucket sizes are exactly the periods recorder.get_statistics offers below a
# day. 5-minute statistics are purged after 10 days; hourly ones are kept.
BUCKETS = {"5min": 300, "hour": 3600}
MAX_5MIN_WINDOW_S = 10 * 86400
# A `today` window can span 25 hours on the autumn DST transition.
TODAY_CAPACITY_S = 25 * 3600


def validate_window(value):
    """`today`, or a duration such as `24h` / `3d`."""
    if isinstance(value, str) and value.strip().lower() == "today":
        return "today"
    period = cv.positive_time_period_seconds(value)
    if period.total_seconds < 600:
        raise cv.Invalid("window must be at least 10 minutes")
    return period


def window_seconds(window) -> int:
    """Seconds the buffer must cover; the DST-safe 25h for `today`."""
    if window == "today":
        return TODAY_CAPACITY_S
    assert isinstance(window, TimePeriod)
    return int(window.total_seconds)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(HaHistory),
        cv.GenerateID(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
        cv.Optional(CONF_WINDOW, default="24h"): validate_window,
        cv.Optional(CONF_BUCKET, default="5min"): cv.one_of(*BUCKETS, lower=True),
        cv.Optional(CONF_RETRY_INTERVAL, default="30s"): cv.positive_time_period_milliseconds,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_time(await cg.get_variable(config[CONF_TIME_ID])))
    cg.add(var.set_retry_interval(config[CONF_RETRY_INTERVAL]))

    # The same defines a YAML `homeassistant.action` with capture_response sets
    # (api/__init__.py): they enable the call_id / wants_response /
    # response_template fields on the request and the response handler on the
    # connection.
    cg.add_define("USE_API_HOMEASSISTANT_SERVICES")
    cg.add_define("USE_API_HOMEASSISTANT_ACTION_RESPONSES")
    cg.add_define("USE_API_HOMEASSISTANT_ACTION_RESPONSES_JSON")
