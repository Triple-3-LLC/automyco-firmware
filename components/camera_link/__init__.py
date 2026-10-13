"""camera_link: the main board's end of the camera link (Serial2, GPIO16/17, 115200).

Installs AutoMyco camera firmware on a stock camera with the stock camera's own updater, and keeps an
AutoMyco camera on the chamber's Wi-Fi. Protocol and states: docs/camera-install.md.
"""
from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch, text_sensor, uart
from esphome.const import CONF_ID, CONF_TRIGGER_ID

CODEOWNERS = ["@aishroom"]
DEPENDENCIES = ["uart", "wifi"]
AUTO_LOAD = ["json", "text_sensor"]

CONF_CAMERA_POWER = "camera_power"
CONF_STATUS = "status"
CONF_FIRMWARE = "firmware"
CONF_ADDRESS = "address"
CONF_RESET_WINDOW = "reset_window"
CONF_LOG_LINES = "log_lines"
CONF_PASSIVE = "passive"
CONF_ON_LINE = "on_line"
CONF_ON_EVENT = "on_event"

camera_link_ns = cg.esphome_ns.namespace("camera_link")
CameraLink = camera_link_ns.class_("CameraLink", cg.Component, uart.UARTDevice)
LineTrigger = camera_link_ns.class_("LineTrigger", automation.Trigger.template(cg.std_string))
EventTrigger = camera_link_ns.class_("EventTrigger", automation.Trigger.template(cg.std_string))

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(CameraLink),
            cv.Required(CONF_CAMERA_POWER): cv.use_id(switch.Switch),
            cv.Optional(CONF_STATUS): text_sensor.text_sensor_schema(icon="mdi:camera-wireless"),
            cv.Optional(CONF_FIRMWARE): text_sensor.text_sensor_schema(
                icon="mdi:chip", entity_category="diagnostic"
            ),
            cv.Optional(CONF_ADDRESS): text_sensor.text_sensor_schema(
                icon="mdi:ip-network", entity_category="diagnostic"
            ),
            # Camera Reset Login only works this long after the chamber's power-on (physical presence).
            cv.Optional(CONF_RESET_WINDOW, default="30min"): cv.positive_time_period_milliseconds,
            # Bench: log every line on the link (Wi-Fi credential lines redacted).
            cv.Optional(CONF_LOG_LINES, default=False): cv.boolean,
            # Bench: never read or write the link (camera_backup_bridge.yaml hands it to esptool).
            cv.Optional(CONF_PASSIVE, default=False): cv.boolean,
            # Every line the camera sends, as `line` (std::string). Other packages that need the camera's
            # console (common/stock-camera-timelapse.yaml) read it here: camera_link is the only reader of
            # cam_uart. Lines can carry the camera's echo of the Wi-Fi password: never log them unredacted.
            cv.Optional(CONF_ON_LINE): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(LineTrigger)}
            ),
            # Things worth one Activity log entry (a camera power cycle), as `message` (std::string).
            cv.Optional(CONF_ON_EVENT): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(EventTrigger)}
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema("camera_link", baud_rate=115200)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    cg.add(var.set_camera_power(await cg.get_variable(config[CONF_CAMERA_POWER])))
    cg.add(var.set_reset_window(config[CONF_RESET_WINDOW].total_milliseconds))
    cg.add(var.set_log_lines(config[CONF_LOG_LINES]))
    cg.add(var.set_passive(config[CONF_PASSIVE]))
    for conf in config.get(CONF_ON_LINE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "line")], conf)
    for conf in config.get(CONF_ON_EVENT, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "message")], conf)
    for key, setter in ((CONF_STATUS, "set_status_sensor"), (CONF_FIRMWARE, "set_firmware_sensor"),
                        (CONF_ADDRESS, "set_address_sensor")):
        if key in config:
            cg.add(getattr(var, setter)(await text_sensor.new_text_sensor(config[key])))
