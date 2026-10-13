"""frame_uploader: every `update_interval`, grab a JPEG from an esp32_camera and POST it over HTTPS, either
to the frame-ingest Lambda (cloud/frame-ingest, configured url/token/grow) or, in remote mode, to
<url>/api/device/remote/frame in the chamber's name with credentials the chamber sends at runtime
(chamber_link on_remote). No AWS keys on the device.

With camera_http_id, dark frames are lit: the sensor's exposure index (auto-exposure lines x gain) is
read before each upload and, from dark_exposure up, camera_http fades the camera LED up for about
1.2-1.5 s (never more than 2.5 s) around the capture, then puts it back (frame_light.h)."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32
from esphome.components.camera_http import CameraHttpServer
from esphome.components.esp32_camera import ESP32Camera
from esphome.const import CONF_ID, CONF_URL

CODEOWNERS = ["@aishroom"]
MULTI_CONF = True   # e.g. the owner camera: frame-ingest timelapse + remote frames (camera.yaml)
DEPENDENCIES = ["esp32_camera", "wifi"]

CONF_CAMERA_ID = "camera_id"
CONF_TOKEN = "token"
CONF_GROW = "grow"
CONF_CAMERA_HTTP_ID = "camera_http_id"
CONF_DARK_EXPOSURE = "dark_exposure"
CONF_REMOTE_INTERVAL = "remote_interval"

frame_uploader_ns = cg.esphome_ns.namespace("frame_uploader")
FrameUploader = frame_uploader_ns.class_("FrameUploader", cg.PollingComponent)


def _grow_id(value):
    value = cv.string_strict(value)
    if not value or len(value) > 63 or not all(c.islower() or c.isdigit() or c == "-" for c in value):
        raise cv.Invalid("grow must be 1-63 chars of [a-z0-9-] (matches the Lambda's check)")
    return value


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(FrameUploader),
        cv.GenerateID(CONF_CAMERA_ID): cv.use_id(ESP32Camera),
        # The frame-ingest target (cloud/frame-ingest): all three, or none for a camera that only uploads
        # in remote mode (set at runtime by chamber_link on_remote; docs/remote-access.md "Camera").
        cv.Optional(CONF_URL): cv.url,
        cv.Optional(CONF_TOKEN): cv.All(cv.string_strict, cv.Length(min=16)),
        cv.Optional(CONF_GROW): _grow_id,
        # Lit frames: the LED's driver (camera_http, which has light/light_enable). Without it, frames
        # are never lit. The "Light Timelapse Frames" switch turns it on/off at runtime (set_light_frames).
        cv.Optional(CONF_CAMERA_HTTP_ID): cv.use_id(CameraHttpServer),
        # Exposure index (OV2640 AEC lines x analog gain) from which a scene counts as dark. A full
        # UXGA frame is about 1,200 lines, so 1000 = auto-exposure has nearly run out of shutter at 1x
        # gain. The Frame Exposure sensor shows the live value for tuning on the bench.
        cv.Optional(CONF_DARK_EXPOSURE, default=1000): cv.int_range(min=1, max=100000),
        # Remote mode: how often to upload when nobody is watching (every 30 s while someone is). Set
        # update_interval shorter (e.g. 15s): it's how often the uploader checks whether one is due.
        cv.Optional(CONF_REMOTE_INTERVAL, default="300s"): cv.All(
            cv.positive_time_period_milliseconds, cv.Range(min=cv.TimePeriod(seconds=30))
        ),
    }
).extend(cv.polling_component_schema("300s"))


def _all_or_none(config):
    given = [k for k in (CONF_URL, CONF_TOKEN, CONF_GROW) if k in config]
    if given and len(given) != 3:
        raise cv.Invalid("frame-ingest needs url, token and grow together (or none of them for remote mode only)")
    return config


CONFIG_SCHEMA = cv.All(CONFIG_SCHEMA, _all_or_none)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cam = await cg.get_variable(config[CONF_CAMERA_ID])
    cg.add(var.set_camera(cam))
    cg.add(var.set_remote_interval(config[CONF_REMOTE_INTERVAL].total_milliseconds))
    if CONF_URL in config:
        cg.add(var.set_url(config[CONF_URL].rstrip("/")))
        cg.add(var.set_token(config[CONF_TOKEN]))
        cg.add(var.set_grow(config[CONF_GROW]))
    if CONF_CAMERA_HTTP_ID in config:
        cg.add(var.set_camera_http(await cg.get_variable(config[CONF_CAMERA_HTTP_ID])))
    cg.add(var.set_dark_threshold(config[CONF_DARK_EXPOSURE]))
    esp32.include_builtin_idf_component("esp_http_client")
    esp32.include_builtin_idf_component("esp-tls")
    esp32.require_certificate_bundle()  # verify the Lambda URL cert (Amazon CAs are in the common bundle)
