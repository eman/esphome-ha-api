"""A numeric field of an action response, as an ESPHome sensor."""

import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv

from .. import TARGET_SCHEMA, Target, ha_action_ns, register_target

DEPENDENCIES = ["ha_action"]

HaActionSensor = ha_action_ns.class_("HaActionSensor", sensor.Sensor, Target, cg.Component)

CONFIG_SCHEMA = sensor.sensor_schema(HaActionSensor).extend(TARGET_SCHEMA).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = await sensor.new_sensor(config)
    await cg.register_component(var, config)
    await register_target(var, config)
