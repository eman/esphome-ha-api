"""ha_action — call Home Assistant actions from ESPHome, on a schedule.

ESPHome can already call an action and capture its response, but only as an
automation action: you fire it yourself, it stores nothing, and the parsed
document dies when your lambda returns. So the usual way to get a forecast onto
a panel is to build template sensors in Home Assistant that flatten
`weather.get_forecasts` into attributes. This makes the round trip declarative
instead.

Also the home of the shared Home Assistant action transport
(`action_client.h`), which `ha_history` auto-loads this component to get.
MULTI_CONF_NO_DEFAULT is what makes that free: an auto-loaded multi-conf
component collapses to an empty list (config.py:594), so `ha_history` pulls in
the source files without declaring any requests of its own.
"""

from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ACTION, CONF_DATA, CONF_ID, CONF_TIMEOUT, CONF_UPDATE_INTERVAL
from esphome.cpp_types import arduino_json_ns

from .path import add_path, compile_path

CODEOWNERS = ["@eman"]
DEPENDENCIES = ["api"]
# api's own AUTO_LOAD only adds json when a YAML `homeassistant.action` sets
# capture_response; we turn action responses on ourselves, so we add it.
AUTO_LOAD = ["json"]
MULTI_CONF = True
MULTI_CONF_NO_DEFAULT = True

CONF_TEMPLATE = "template"
CONF_RESPONSE_TEMPLATE = "response_template"
CONF_RETAIN = "retain"
CONF_RETRY_INTERVAL = "retry_interval"
CONF_ON_RESPONSE = "on_response"
CONF_ON_ERROR = "on_error"
CONF_CARRIER_ACTION = "carrier_action"
CONF_CARRIER_DATA = "carrier_data"
CONF_HA_ACTION_ID = "ha_action_id"
CONF_PATH = "path"

ha_action_ns = cg.esphome_ns.namespace("ha_action")
HaAction = ha_action_ns.class_("HaAction", cg.Component)
Target = ha_action_ns.class_("Target")
JsonVariantConst = arduino_json_ns.class_("JsonVariantConst")

# `template:` renders arbitrary server-side Jinja and sends back the result.
# Home Assistant only renders a response_template for an action that actually
# returns a response (manager.py, inside the need_response_data branch), and
# there is no generic homeassistant.* action that does. So one has to be
# carried, and this is the cheapest universally available carrier: recorder
# ships in default_config, a statistic id matching nothing does no real database
# work, and a start_time fixed in the future means no clock is needed - which is
# what keeps `template:` free of a `time:` dependency.
CARRIER_ACTION = "recorder.get_statistics"
CARRIER_DATA = {
    "start_time": "2099-01-01T00:00:00+00:00",
    "statistic_ids": "ha_action.carrier_matches_nothing",
    "period": "5minute",
    "types": "mean",
}

KEY_VALUE_SCHEMA = cv.Schema({cv.string: cv.templatable(cv.string_strict)})


def _validate(config):
    if CONF_TEMPLATE in config:
        if CONF_RESPONSE_TEMPLATE in config:
            raise cv.Invalid(
                f"`{CONF_TEMPLATE}` is itself a response template. Use one or the other.",
                path=[CONF_RESPONSE_TEMPLATE],
            )
        if config[CONF_DATA]:
            raise cv.Invalid(
                f"`{CONF_DATA}` has no meaning with `{CONF_TEMPLATE}`: the action behind it is only a "
                f"carrier. Put what you need inside the template, which is rendered by Home Assistant "
                f"with states(), state_attr() and the rest of its template environment in scope.",
                path=[CONF_DATA],
            )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(HaAction),
            cv.Exclusive(CONF_ACTION, CONF_ACTION): cv.string_strict,
            cv.Exclusive(CONF_TEMPLATE, CONF_ACTION): cv.templatable(cv.string),
            cv.Optional(CONF_DATA, default={}): KEY_VALUE_SCHEMA,
            cv.Optional(CONF_RESPONSE_TEMPLATE): cv.templatable(cv.string),
            # Omitted means "once per API connection", which is right for
            # anything that only changes when Home Assistant restarts.
            cv.Optional(CONF_UPDATE_INTERVAL): cv.All(
                cv.positive_not_null_time_period, cv.positive_time_period_milliseconds
            ),
            cv.Optional(CONF_RETRY_INTERVAL, default="30s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_TIMEOUT, default="20s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_RETAIN, default=False): cv.boolean,
            cv.Optional(CONF_CARRIER_ACTION, default=CARRIER_ACTION): cv.string_strict,
            cv.Optional(CONF_CARRIER_DATA, default=CARRIER_DATA): KEY_VALUE_SCHEMA,
            cv.Optional(CONF_ON_RESPONSE): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_ERROR): automation.validate_automation(single=True),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.has_exactly_one_key(CONF_ACTION, CONF_TEMPLATE),
    _validate,
)


def require_action_responses() -> None:
    """Turn on the API's action-response machinery.

    These are the same defines a YAML `homeassistant.action` with
    `capture_response: true` sets (api/__init__.py:615-629): they enable the
    call_id / wants_response / response_template fields on the request and the
    response handler on the connection. Every component that uses ActionClient
    calls this from its own `to_code`, because a multi-conf component with no
    entries never has its `to_code` run at all.
    """
    cg.add_define("USE_API_HOMEASSISTANT_SERVICES")
    cg.add_define("USE_API_HOMEASSISTANT_ACTION_RESPONSES")
    cg.add_define("USE_API_HOMEASSISTANT_ACTION_RESPONSES_JSON")


async def register_target(var, config) -> None:
    """Wire an entity platform's compiled path onto its request."""
    parent = await cg.get_variable(config[CONF_HA_ACTION_ID])
    cg.add(parent.add_target(var))
    add_path(var, config[CONF_PATH])


TARGET_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_HA_ACTION_ID): cv.use_id(HaAction),
        cv.Required(CONF_PATH): compile_path,
    }
)


async def to_code(config):
    require_action_responses()

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_name(str(config[CONF_ID])))

    if CONF_TEMPLATE in config:
        cg.add(var.set_action(config[CONF_CARRIER_ACTION]))
        data = config[CONF_CARRIER_DATA]
        response_template = config[CONF_TEMPLATE]
    else:
        cg.add(var.set_action(config[CONF_ACTION]))
        data = config[CONF_DATA]
        response_template = config.get(CONF_RESPONSE_TEMPLATE)

    for key, value in data.items():
        templ = await cg.templatable(value, [], cg.std_string)
        cg.add(var.add_data(key, templ))

    if response_template is not None:
        templ = await cg.templatable(response_template, [], cg.std_string)
        cg.add(var.set_response_template(templ))

    if (interval := config.get(CONF_UPDATE_INTERVAL)) is not None:
        cg.add(var.set_update_interval(interval))
    cg.add(var.set_retry_interval(config[CONF_RETRY_INTERVAL]))
    cg.add(var.set_timeout(config[CONF_TIMEOUT]))
    cg.add(var.set_retain(config[CONF_RETAIN]))

    if on_response := config.get(CONF_ON_RESPONSE):
        await automation.build_automation(
            var.get_response_trigger(), [(JsonVariantConst, "response")], on_response
        )
    if on_error := config.get(CONF_ON_ERROR):
        await automation.build_automation(var.get_error_trigger(), [(cg.std_string, "error")], on_error)
