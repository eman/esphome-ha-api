"""The Home Assistant action transport, shared by ha_action and ha_history.

No YAML surface: this component exists only so both of those can share one
implementation of the wire without depending on each other. ESPHome copies
whole component directories, so a shared file living inside `ha_action` would
drag that component's entire implementation into every `ha_history` build.

See action_client.h for what the transport does and why the built-in
`homeassistant.action` cannot be used for scheduled requests.
"""

import esphome.codegen as cg
import esphome.config_validation as cv

CODEOWNERS = ["@eman"]
DEPENDENCIES = ["api"]
# api's own AUTO_LOAD only adds json when a YAML `homeassistant.action` sets
# capture_response; we turn action responses on ourselves, so we add it.
AUTO_LOAD = ["json"]

ha_api_core_ns = cg.esphome_ns.namespace("ha_api_core")
ActionClient = ha_api_core_ns.class_("ActionClient")

CONFIG_SCHEMA = cv.Schema({})


def require_action_responses() -> None:
    """Turn on the API's action-response machinery.

    These are the same defines a YAML `homeassistant.action` with
    `capture_response: true` sets (api/__init__.py:615-629): they enable the
    call_id / wants_response / response_template fields on the request and the
    response handler on the connection.

    Called from this component's own `to_code`, which runs whenever anything
    auto-loads it - so `ha_action` and `ha_history` get the defines by depending
    on the transport, without either having to remember to ask.
    """
    cg.add_define("USE_API_HOMEASSISTANT_SERVICES")
    cg.add_define("USE_API_HOMEASSISTANT_ACTION_RESPONSES")
    cg.add_define("USE_API_HOMEASSISTANT_ACTION_RESPONSES_JSON")


async def to_code(config):
    require_action_responses()
