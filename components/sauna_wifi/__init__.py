"""Wi-Fi with a fallback access point that does not disturb its own clients.

Joins the configured network when it is there and runs the device's own
access point when it is not, like ESPHome's `wifi: ap:` fallback. The
difference: on an ESP32 ESPHome keeps scanning for the network the whole
time the fallback AP is up, and every scan takes the radio off the AP's
channel. Here the search pauses once the AP has been up for `search_time`,
and resumes every `retry_interval` - preferably while nobody is joined to
the AP, but never later than `max_defer`.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@arctus"]
DEPENDENCIES = ["wifi"]

CONF_SEARCH_TIME = "search_time"
CONF_RETRY_INTERVAL = "retry_interval"
CONF_MAX_DEFER = "max_defer"

sauna_wifi_ns = cg.esphome_ns.namespace("sauna_wifi")
SaunaWifi = sauna_wifi_ns.class_("SaunaWifi", cg.Component)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SaunaWifi),
            cv.Optional(
                CONF_SEARCH_TIME, default="2min"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_RETRY_INTERVAL, default="15min"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_MAX_DEFER, default="60min"
            ): cv.positive_time_period_milliseconds,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_search_time(config[CONF_SEARCH_TIME]))
    cg.add(var.set_retry_interval(config[CONF_RETRY_INTERVAL]))
    cg.add(var.set_max_defer(config[CONF_MAX_DEFER]))
