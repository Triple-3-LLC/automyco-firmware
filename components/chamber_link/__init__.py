"""chamber_link: the AutoMyco camera's end of the camera link (its UART0, which the logger also uses).

Speaks the stock camera protocol, so the main board treats a stock and an AutoMyco camera alike:
  camera -> main  {"status":"ready","version":"automyco-camera/<version>"}   at boot and on {"cmd":"status"}
  camera -> main  {"status":"online","ip":"<ipv4>"}                          each time Wi-Fi connects
  main -> camera  {"ssid":"...","password":"..."}                            Wi-Fi (saved; never logged)
  main -> camera  {"cmd":"reset_login"}                                      clear the camera_http login, reboot
  main -> camera  {"ota":{"version":"automyco-camera/<v>","hash":"<sha256>"}}  update to that published release
                                                                             (camera_update; progress comes back as
                                                                             {"status":"ota",...} plus the stock
                                                                             updater's plain-text lines)
  main -> camera  {"cmd":"open_api"}                                         restart once with the Home Assistant
                                                                             adoption window open (on_command;
                                                                             camera-public.yaml)
  camera -> main  {"status":"power_cycle","reason":"..."}                    frame watchdog: please cut my power
                                                                             (a wedged OV2640 needs it; 2026-10-09)
  main -> camera  {"cmd":"remote","url":"https://...","id":"<32 hex>","key":"<64 hex>"}
                  or {"cmd":"remote","off":true}                             remote frame uploads in the chamber's
                                                                             name (on_remote; RAM only, key never
                                                                             logged; docs/remote-access.md)
See docs/camera-install.md.
"""
from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components.camera_http import CameraHttpServer
from esphome.components.esp32_camera import ESP32Camera
from esphome.const import CONF_ID, CONF_TRIGGER_ID, CONF_VERSION

CODEOWNERS = ["@aishroom"]
DEPENDENCIES = ["logger", "wifi"]
AUTO_LOAD = ["json"]

CONF_CAMERA_HTTP_ID = "camera_http_id"
CONF_CAMERA_ID = "camera_id"
CONF_WATCHDOG_TIMEOUT = "watchdog_timeout"
CONF_ON_REMOTE = "on_remote"
CONF_ON_COMMAND = "on_command"

chamber_link_ns = cg.esphome_ns.namespace("chamber_link")
ChamberLink = chamber_link_ns.class_("ChamberLink", cg.Component)
CommandTrigger = chamber_link_ns.class_("CommandTrigger", automation.Trigger.template(cg.std_string))
RemoteTrigger = chamber_link_ns.class_(
    "RemoteTrigger", automation.Trigger.template(cg.std_string, cg.std_string, cg.std_string)
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ChamberLink),
        cv.Required(CONF_VERSION): cv.All(cv.string_strict, cv.Length(min=1, max=24)),
        cv.Optional(CONF_CAMERA_HTTP_ID): cv.use_id(CameraHttpServer),
        # Frame watchdog: with the camera's idle frames (idle_framerate) arriving every few seconds, no
        # valid frame for this long means the sensor is wedged, and only a power cut fixes that.
        cv.Optional(CONF_CAMERA_ID): cv.use_id(ESP32Camera),
        cv.Optional(CONF_WATCHDOG_TIMEOUT, default="60s"): cv.All(
            cv.positive_time_period_milliseconds, cv.Range(min=cv.TimePeriod(seconds=30))
        ),
        # Variables: url, device_id, key (all empty = stop uploading).
        cv.Optional(CONF_ON_REMOTE): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(RemoteTrigger)}
        ),
        # Variable: cmd. Any {"cmd": ...} from the chamber that isn't status, remote or reset_login.
        cv.Optional(CONF_ON_COMMAND): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(CommandTrigger)}
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_version(config[CONF_VERSION]))
    if CONF_CAMERA_ID in config:
        cg.add(var.set_camera(await cg.get_variable(config[CONF_CAMERA_ID])))
        cg.add(var.set_watchdog_timeout(config[CONF_WATCHDOG_TIMEOUT].total_milliseconds))
    if CONF_CAMERA_HTTP_ID in config:
        cg.add(var.set_camera_http(await cg.get_variable(config[CONF_CAMERA_HTTP_ID])))
    for conf in config.get(CONF_ON_REMOTE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(cg.std_string, "url"), (cg.std_string, "device_id"), (cg.std_string, "key")], conf
        )
    for conf in config.get(CONF_ON_COMMAND, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "cmd")], conf)
