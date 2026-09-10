"""A field of an action response, as an ESPHome text sensor."""

import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv

from .. import TARGET_SCHEMA, Target, ha_action_ns, register_target

DEPENDENCIES = ["ha_action"]

HaActionTextSensor = ha_action_ns.class_(
    "HaActionTextSensor", text_sensor.TextSensor, Target, cg.Component
)

CONFIG_SCHEMA = (
    text_sensor.text_sensor_schema(HaActionTextSensor).extend(TARGET_SCHEMA).extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await text_sensor.new_text_sensor(config)
    await cg.register_component(var, config)
    await register_target(var, config)
