"""The `ha_history` sensor platform.

Accepts everything `platform: homeassistant` does - the schema and its helper
are reused from that component - plus a `history:` block and two triggers.
"""

from esphome import automation
import esphome.codegen as cg
from esphome.components import sensor
from esphome.components.homeassistant import (
    HOME_ASSISTANT_IMPORT_SCHEMA,
    setup_home_assistant_entity,
    validate_entity_domain,
)
import esphome.config_validation as cv
from esphome.const import CONF_ATTRIBUTE
from esphome.core import CORE
import esphome.final_validate as fv

from .. import (
    BUCKETS,
    CONF_BUCKET,
    CONF_HA_HISTORY_ID,
    CONF_WINDOW,
    MAX_5MIN_WINDOW_S,
    HaHistory,
    ha_history_ns,
    validate_window,
    window_seconds,
)

DEPENDENCIES = ["api"]
AUTO_LOAD = ["ha_history", "json"]

CONF_HISTORY = "history"
CONF_STATISTIC = "statistic"
CONF_ON_HISTORY_LOADED = "on_history_loaded"
CONF_ON_HISTORY_UPDATE = "on_history_update"

HaHistorySensor = ha_history_ns.class_("HaHistorySensor", sensor.Sensor, cg.Component)
WindowKind = ha_history_ns.enum("WindowKind", is_class=True)
Statistic = ha_history_ns.enum("Statistic", is_class=True)

# `sum` is deliberately absent: it cannot be extended live without the previous
# running total. `change` covers the metered-sensor case.
STATISTICS = {
    "mean": Statistic.MEAN,
    "min": Statistic.MIN,
    "max": Statistic.MAX,
    "state": Statistic.STATE,
    "change": Statistic.CHANGE,
}

# The reply must fit one API frame (32 KiB), and exceeding it drops the
# connection - which HA then re-establishes, so the device would ask again and
# drop it again. ~16 bytes per point on the wire, plus an ArduinoJson node per
# value in RAM; without PSRAM that second copy is the tighter limit.
MAX_POINTS_PSRAM = 1000
MAX_POINTS_INTERNAL = 500

HISTORY_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_WINDOW): validate_window,
        cv.Optional(CONF_BUCKET): cv.one_of(*BUCKETS, lower=True),
        cv.Optional(CONF_STATISTIC, default="mean"): cv.enum(STATISTICS, lower=True),
    }
)


def _no_attribute(config):
    if CONF_ATTRIBUTE in config:
        raise cv.Invalid(
            "ha_history cannot backfill an attribute: Home Assistant keeps statistics "
            "only for the entity's state. Use `platform: homeassistant` for attributes."
        )
    return config


CONFIG_SCHEMA = cv.All(
    sensor.sensor_schema(HaHistorySensor, accuracy_decimals=1)
    .extend(HOME_ASSISTANT_IMPORT_SCHEMA)
    .extend(
        {
            cv.GenerateID(CONF_HA_HISTORY_ID): cv.use_id(HaHistory),
            cv.Optional(CONF_HISTORY, default={}): HISTORY_SCHEMA,
            cv.Optional(CONF_ON_HISTORY_LOADED): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_HISTORY_UPDATE): automation.validate_automation(single=True),
        }
    ),
    validate_entity_domain("ha_history", ["sensor"]),
    _no_attribute,
    cv.only_on_esp32,
)


def _resolve(config, hub_config):
    """The sensor's effective window/bucket, falling back to the hub's.

    Both are already validated (`today` or a TimePeriod); when the hub was
    auto-loaded its defaults are filled in by its own schema.
    """
    hist = config[CONF_HISTORY]
    window = hist.get(CONF_WINDOW, hub_config.get(CONF_WINDOW, validate_window("24h")))
    bucket = hist.get(CONF_BUCKET, hub_config.get(CONF_BUCKET, "5min"))
    return window, BUCKETS[bucket]


def _final_validate(config):
    full = fv.full_config.get()
    hub_config = full.get("ha_history") or {}
    window, bucket_s = _resolve(config, hub_config)
    span_s = window_seconds(window)

    if bucket_s == 300 and span_s > MAX_5MIN_WINDOW_S:
        raise cv.Invalid(
            "Home Assistant purges 5-minute statistics after 10 days; use `bucket: hour` "
            "for a window this long.",
            path=[CONF_HISTORY],
        )

    points = span_s // bucket_s
    cap = MAX_POINTS_PSRAM if "psram" in full else MAX_POINTS_INTERNAL
    if points > cap:
        raise cv.Invalid(
            f"{points} points would not fit the Home Assistant API frame limit (max {cap} "
            f"{'with' if cap == MAX_POINTS_PSRAM else 'without'} PSRAM). Use a shorter window or "
            f"`bucket: hour`.",
            path=[CONF_HISTORY],
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = await sensor.new_sensor(config)
    await cg.register_component(var, config)
    setup_home_assistant_entity(var, config)

    hub = await cg.get_variable(config[CONF_HA_HISTORY_ID])
    cg.add(var.set_parent(hub))
    cg.add(hub.register_sensor(var))

    window, bucket_s = _resolve(config, CORE.config.get("ha_history") or {})
    span_s = window_seconds(window)
    kind = WindowKind.TODAY if window == "today" else WindowKind.ROLLING
    cg.add(var.set_window(kind, span_s if window != "today" else 86400))
    cg.add(var.set_bucket(bucket_s))
    cg.add(var.set_statistic(config[CONF_HISTORY][CONF_STATISTIC]))
    # +2: the bucket in progress and one of slack at the window edge.
    cg.add(var.set_capacity(span_s // bucket_s + 2))

    if CONF_ON_HISTORY_LOADED in config:
        await automation.build_automation(var.get_loaded_trigger(), [], config[CONF_ON_HISTORY_LOADED])
    if CONF_ON_HISTORY_UPDATE in config:
        await automation.build_automation(var.get_update_trigger(), [], config[CONF_ON_HISTORY_UPDATE])
