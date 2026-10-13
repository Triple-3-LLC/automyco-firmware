"""camera_update: the AutoMyco camera updates itself to a published camera release.

Requests come from the main board over the camera link (the stock camera's own command,
{"ota":{"version":"automyco-camera/<v>","hash":"<sha256>"}}, via chamber_link) and from the iOS app
(POST :8080/update {"version","hash"[,"force"]}, behind the camera's login, via camera_http). The camera
builds the URL itself, https://<host>/fw/<sha256>.bin, follows no redirects, and boots the image only when
its SHA-256 matches the request. It refuses the same or an older version unless forced (the app only).

  camera_update:
    version: ${camera_version}
    chamber_link_id: ...      # the main board's requests and the progress lines back
    camera_http_id: ...       # the app's requests; the stream pauses during the update
    on_begin:  [...]          # pause frame uploads
    on_abort:  [...]          # resume them (a successful update reboots instead)

Rules (host-tested): update_logic.h. Flow: docs/camera-install.md "Updating an AutoMyco camera".
"""
from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32
from esphome.components.camera_http import CameraHttpServer
from esphome.components.chamber_link import ChamberLink
from esphome.const import CONF_ID, CONF_TRIGGER_ID, CONF_VERSION

CODEOWNERS = ["@aishroom"]
DEPENDENCIES = ["wifi", "ota", "chamber_link"]
AUTO_LOAD = ["json", "md5"]

CONF_HOST = "host"
CONF_CHAMBER_LINK_ID = "chamber_link_id"
CONF_CAMERA_HTTP_ID = "camera_http_id"
CONF_ON_BEGIN = "on_begin"
CONF_ON_ABORT = "on_abort"

camera_update_ns = cg.esphome_ns.namespace("camera_update")
CameraUpdate = camera_update_ns.class_("CameraUpdate", cg.Component)
BeginTrigger = camera_update_ns.class_("BeginTrigger", automation.Trigger.template())
AbortTrigger = camera_update_ns.class_("AbortTrigger", automation.Trigger.template())


def _hostname(value):
    value = cv.string_strict(value)
    if (
        not value
        or len(value) > 64
        or value.startswith(".")
        or value.endswith(".")
        or ".." in value
        or not all(c.isalnum() or c in ".-" for c in value)
    ):
        raise cv.Invalid("host must be a bare hostname, e.g. api.automyco.com (no scheme, path or port)")
    return value


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(CameraUpdate),
        cv.Required(CONF_VERSION): cv.All(cv.string_strict, cv.Length(min=1, max=24)),
        # The vendor API (cloud/vendor-api) on its ACM certificate: serves only published camera releases,
        # by hash, re-hashed before sending.
        cv.Optional(CONF_HOST, default="api.automyco.com"): _hostname,
        cv.GenerateID(CONF_CHAMBER_LINK_ID): cv.use_id(ChamberLink),
        cv.Optional(CONF_CAMERA_HTTP_ID): cv.use_id(CameraHttpServer),
        cv.Optional(CONF_ON_BEGIN): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(BeginTrigger)}
        ),
        cv.Optional(CONF_ON_ABORT): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(AbortTrigger)}
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_version(config[CONF_VERSION]))
    cg.add(var.set_host(config[CONF_HOST]))
    cg.add(var.set_chamber_link(await cg.get_variable(config[CONF_CHAMBER_LINK_ID])))
    if CONF_CAMERA_HTTP_ID in config:
        cg.add(var.set_camera_http(await cg.get_variable(config[CONF_CAMERA_HTTP_ID])))
    for conf in config.get(CONF_ON_BEGIN, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)
    for conf in config.get(CONF_ON_ABORT, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)
    esp32.include_builtin_idf_component("esp_http_client")
    esp32.include_builtin_idf_component("esp-tls")
    esp32.require_certificate_bundle()  # Amazon roots, for api.automyco.com's ACM certificate
