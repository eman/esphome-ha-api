"""A button that re-runs the history backfill.

Exists for one moment in particular: the user has just enabled "Allow the
device to perform Home Assistant actions" in Home Assistant, and the hub has
already given up waiting for a reply that was never going to come.
"""

import esphome.codegen as cg
from esphome.components import button
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_CONFIG

from .. import CONF_HA_HISTORY_ID, HaHistory, ha_history_ns

DEPENDENCIES = ["ha_history"]

HaHistoryReloadButton = ha_history_ns.class_("HaHistoryReloadButton", button.Button, cg.Component)

CONFIG_SCHEMA = (
    button.button_schema(
        HaHistoryReloadButton,
        icon="mdi:history",
        entity_category=ENTITY_CATEGORY_CONFIG,
    )
    .extend({cv.GenerateID(CONF_HA_HISTORY_ID): cv.use_id(HaHistory)})
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await button.new_button(config)
    await cg.register_component(var, config)
    cg.add(var.set_parent(await cg.get_variable(config[CONF_HA_HISTORY_ID])))
