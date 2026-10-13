"""camera_http: password-protected MJPEG stream + JPEG snapshot for an esp32_camera.

Replaces ESPHome's esp32_camera_web_server (which has no authentication). Serves on its own
esp_http_server instance:
  GET /stream    multipart/x-mixed-replace MJPEG
  GET /snapshot  one image/jpeg
Both require HTTP Basic auth (constant-time compare); 401 carries WWW-Authenticate.

The login is either fixed in the config (username/password: the personal build) or claimed once
(claim_window: the public build). In claim mode the camera starts locked with a random login, and
  GET  /claim  -> {"claimed": bool, "open": bool}
  POST /claim  {"username", "password"}  (only while unclaimed, within claim_window of boot)
sets the login, saves it in flash and applies it to web_server too. See docs/camera-install.md.
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32, light, switch
from esphome.components.esp32_camera import ESP32Camera
from esphome.const import CONF_ID, CONF_PASSWORD, CONF_PORT, CONF_USERNAME

CODEOWNERS = ["@aishroom"]
DEPENDENCIES = ["esp32_camera", "network"]
AUTO_LOAD = ["json"]

CONF_CAMERA_ID = "camera_id"
CONF_CLAIM_WINDOW = "claim_window"
CONF_LIGHT = "light"
CONF_LIGHT_ENABLE = "light_enable"

camera_http_ns = cg.esphome_ns.namespace("camera_http")
CameraHttpServer = camera_http_ns.class_("CameraHttpServer", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(CameraHttpServer),
        cv.GenerateID(CONF_CAMERA_ID): cv.use_id(ESP32Camera),
        cv.Optional(CONF_PORT, default=8080): cv.port,
        cv.Inclusive(CONF_USERNAME, "login"): cv.All(cv.string_strict, cv.Length(min=1)),
        cv.Inclusive(CONF_PASSWORD, "login"): cv.All(cv.string_strict, cv.Length(min=8)),
        # The camera's LED, lit only while someone watches /stream or a /snapshot is taken, and only
        # when light_enable (the owner's "Camera Light" preference) is on. Off otherwise, to save power.
        cv.Inclusive(CONF_LIGHT, "light"): cv.use_id(light.LightState),
        cv.Inclusive(CONF_LIGHT_ENABLE, "light"): cv.use_id(switch.Switch),
        cv.Optional(CONF_CLAIM_WINDOW): cv.All(
            cv.positive_time_period_milliseconds, cv.Range(min=cv.TimePeriod(minutes=1))
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


def _login_xor_claim(config):
    has_login = CONF_USERNAME in config
    if has_login == (CONF_CLAIM_WINDOW in config):
        raise cv.Invalid("set either username + password (fixed login) or claim_window (claimed login)")
    return config


CONFIG_SCHEMA = cv.All(CONFIG_SCHEMA, _login_xor_claim)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cam = await cg.get_variable(config[CONF_CAMERA_ID])
    cg.add(var.set_camera(cam))
    cg.add(var.set_port(config[CONF_PORT]))
    if CONF_LIGHT in config:
        cg.add(var.set_light(await cg.get_variable(config[CONF_LIGHT]),
                             await cg.get_variable(config[CONF_LIGHT_ENABLE])))
    if CONF_CLAIM_WINDOW in config:
        cg.add(var.set_claim_window(config[CONF_CLAIM_WINDOW].total_milliseconds))
    else:
        cg.add(var.set_credentials(config[CONF_USERNAME], config[CONF_PASSWORD]))
    esp32.include_builtin_idf_component("esp_http_server")
