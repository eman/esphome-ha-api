"""A field of an action response, as an ESPHome binary sensor."""

import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv

from .. import TARGET_SCHEMA, Target, ha_action_ns, register_target

DEPENDENCIES = ["ha_action"]

CONF_THRESHOLD = "threshold"

HaActionBinarySensor = ha_action_ns.class_(
    "HaActionBinarySensor", binary_sensor.BinarySensor, Target, cg.Component
)

CONFIG_SCHEMA = (
    binary_sensor.binary_sensor_schema(HaActionBinarySensor)
    .extend(TARGET_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
    .extend(
        {
            # Omitted, the JSON value is read as a boolean, which is right when
            # the response really holds one. Given, the value is read as a
            # number and compared - `precipitation` above 0.1 mm, say.
            cv.Optional(CONF_THRESHOLD): cv.float_,
        }
    )
)


async def to_code(config):
    var = await binary_sensor.new_binary_sensor(config)
    await cg.register_component(var, config)
    await register_target(var, config)
    if (threshold := config.get(CONF_THRESHOLD)) is not None:
        cg.add(var.set_threshold(threshold))
