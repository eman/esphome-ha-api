"""ha_action — call Home Assistant actions from ESPHome, on a schedule.

ESPHome can already call an action and capture its response, but only as an
automation action: you fire it yourself, it stores nothing, and the parsed
document dies when your lambda returns. So the usual way to get a forecast onto
a panel is to build template sensors in Home Assistant that flatten
`weather.get_forecasts` into attributes. This makes the round trip declarative
instead.

The wire itself lives in `ha_api_core`, which this component and `ha_history`
both auto-load; neither depends on the other.
"""

from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import (
    CONF_ACTION,
    CONF_DATA,
    CONF_DATA_TEMPLATE,
    CONF_ID,
    CONF_TIMEOUT,
    CONF_UPDATE_INTERVAL,
    CONF_VARIABLES,
)
from esphome.cpp_types import arduino_json_ns

from .path import add_path, compile_path

CODEOWNERS = ["@eman"]
DEPENDENCIES = ["api"]
# ha_api_core carries the transport, and sets the API defines it needs.
AUTO_LOAD = ["ha_api_core", "json"]
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
        if config[CONF_DATA] or config[CONF_DATA_TEMPLATE]:
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
            # Rendered as Jinja by Home Assistant and literal_eval'd, which is
            # the only way to pass a value that is not a string - a real list,
            # say, where `data` would give you a one-element list holding it.
            cv.Optional(CONF_DATA_TEMPLATE, default={}): KEY_VALUE_SCHEMA,
            cv.Optional(CONF_VARIABLES, default={}): KEY_VALUE_SCHEMA,
            cv.Optional(CONF_RESPONSE_TEMPLATE): cv.templatable(cv.string),
            # Omitted means "once per API connection", which is right for
            # anything that only changes when Home Assistant restarts. `never`
            # means the request runs only when something asks - a refresh
            # button, or another request's on_response.
            cv.Optional(CONF_UPDATE_INTERVAL): cv.Any(
                cv.one_of("never", lower=True),
                cv.All(cv.positive_not_null_time_period, cv.positive_time_period_milliseconds),
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
    var = cg.new_Pvariable(config[CONF_ID])
    # register_component calls set_update_interval() for any config carrying that
    # key (cpp_helpers.py:245). Ours is not a PollingComponent interval - it also
    # takes `never`, and "omitted" means something specific - so it is withheld
    # here and applied below.
    await cg.register_component(var, {k: v for k, v in config.items() if k != CONF_UPDATE_INTERVAL})
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
        cg.add(var.add_data(key, await cg.templatable(value, [], cg.std_string)))
    for key, value in config[CONF_DATA_TEMPLATE].items():
        cg.add(var.add_data_template(key, await cg.templatable(value, [], cg.std_string)))
    for key, value in config[CONF_VARIABLES].items():
        cg.add(var.add_variable(key, await cg.templatable(value, [], cg.std_string)))

    if response_template is not None:
        templ = await cg.templatable(response_template, [], cg.std_string)
        cg.add(var.set_response_template(templ))

    if (interval := config.get(CONF_UPDATE_INTERVAL)) is not None:
        if interval == "never":
            cg.add(var.set_manual(True))
        else:
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
