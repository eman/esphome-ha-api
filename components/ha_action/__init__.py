"""ha_action — call Home Assistant actions from ESPHome, on a schedule.

Also the home of the shared Home Assistant action transport
(`action_client.h`), which `ha_history` auto-loads this component to get.

MULTI_CONF_NO_DEFAULT is what makes that free: an auto-loaded multi-conf
component collapses to an empty list (config.py:594), so `ha_history` pulls in
the source files without declaring any requests of its own.
"""

import esphome.codegen as cg
import esphome.config_validation as cv

CODEOWNERS = ["@eman"]
DEPENDENCIES = ["api"]
# api's own AUTO_LOAD only adds json when a YAML `homeassistant.action` sets
# capture_response; we turn action responses on ourselves, so we add it.
AUTO_LOAD = ["json"]
MULTI_CONF = True
MULTI_CONF_NO_DEFAULT = True

ha_action_ns = cg.esphome_ns.namespace("ha_action")

CONFIG_SCHEMA = cv.Schema({})


def require_action_responses() -> None:
    """Turn on the API's action-response machinery.

    These are the same defines a YAML `homeassistant.action` with
    `capture_response: true` sets (api/__init__.py:615-629): they enable the
    call_id / wants_response / response_template fields on the request and the
    response handler on the connection. Every component that uses
    ActionClient must call this from its own `to_code`, because a multi-conf
    component with no entries never has its `to_code` run at all.
    """
    cg.add_define("USE_API_HOMEASSISTANT_SERVICES")
    cg.add_define("USE_API_HOMEASSISTANT_ACTION_RESPONSES")
    cg.add_define("USE_API_HOMEASSISTANT_ACTION_RESPONSES_JSON")


async def to_code(config):
    require_action_responses()
