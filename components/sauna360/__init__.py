import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import uart
from esphome.const import CONF_ID

DEPENDENCIES = ["uart"]

sauna360_ns = cg.esphome_ns.namespace("sauna360")
SAUNA360Component = sauna360_ns.class_(
    "SAUNA360Component", cg.Component, uart.UARTDevice
)
Mode = sauna360_ns.enum("SAUNA360Component::Mode")

CONF_SAUNA360_ID = "sauna360_id"

CONF_MODEL = "model"
CONF_FLOW_CONTROL_PIN = "flow_control_pin"
# RS485 proxy (not in upstream): heater on uart_id, panel on panel_uart_id.
CONF_PANEL_UART_ID = "panel_uart_id"
CONF_PANEL_REPLY_TIMEOUT = "panel_reply_timeout"
MODEL_OPTIONS = {
    "pure": cg.RawExpression("esphome::sauna360::SAUNA360Component::Mode::PURE"),
    "combi": cg.RawExpression("esphome::sauna360::SAUNA360Component::Mode::COMBI"),
    "elite": cg.RawExpression("esphome::sauna360::SAUNA360Component::Mode::ELITE"),
    "combi_elite": cg.RawExpression(
        "esphome::sauna360::SAUNA360Component::Mode::COMBI_ELITE"
    ),
}


def _flow_control_pin(value):
    """Accept "false"/false (pin disabled, default) or a GPIO output pin."""
    if value is False:
        return False
    if isinstance(value, str) and value.strip().lower() in ("", "false", "none", "no"):
        return False
    return pins.gpio_output_pin_schema(value)


CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SAUNA360Component),
            cv.Optional(CONF_MODEL, default="pure"): cv.enum(MODEL_OPTIONS, lower=True),
            cv.Optional(CONF_FLOW_CONTROL_PIN, default=False): _flow_control_pin,
            cv.Optional(CONF_PANEL_UART_ID): cv.use_id(uart.UARTComponent),
            cv.Optional(
                CONF_PANEL_REPLY_TIMEOUT, default="60ms"
            ): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(min=cv.TimePeriod(milliseconds=5), max=cv.TimePeriod(milliseconds=150)),
            ),
        }
    )
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "sauna360_uart",
    require_tx=True,
    require_rx=True,
    baud_rate=19200,
    parity="EVEN",
    data_bits=8,
    stop_bits=1,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    if config[CONF_FLOW_CONTROL_PIN]:
        pin = await cg.gpio_pin_expression(config[CONF_FLOW_CONTROL_PIN])
        cg.add(var.set_flow_control_pin(pin))
    cg.add(var.set_mode(config[CONF_MODEL]))
    if CONF_PANEL_UART_ID in config:
        panel = await cg.get_variable(config[CONF_PANEL_UART_ID])
        cg.add(var.set_panel_uart(panel))
        cg.add(
            var.set_panel_reply_timeout_ms(
                int(config[CONF_PANEL_REPLY_TIMEOUT].total_milliseconds)
            )
        )
