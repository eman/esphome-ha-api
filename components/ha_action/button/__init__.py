"""A button that re-runs one request.

Exists for one moment in particular: you have just enabled "Allow the device to
perform Home Assistant actions" and the request has already stopped retrying.
"""

import esphome.codegen as cg
from esphome.components import button
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_CONFIG

from .. import CONF_HA_ACTION_ID, HaAction, ha_action_ns

DEPENDENCIES = ["ha_action"]

HaActionRefreshButton = ha_action_ns.class_("HaActionRefreshButton", button.Button, cg.Component)

CONFIG_SCHEMA = (
    button.button_schema(
        HaActionRefreshButton,
        icon="mdi:refresh",
        entity_category=ENTITY_CATEGORY_CONFIG,
    )
    .extend({cv.GenerateID(CONF_HA_ACTION_ID): cv.use_id(HaAction)})
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await button.new_button(config)
    await cg.register_component(var, config)
    cg.add(var.set_parent(await cg.get_variable(config[CONF_HA_ACTION_ID])))
