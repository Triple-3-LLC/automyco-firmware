"""flash_dumper: read-only HTTP dump of the app slot the chip is NOT running.

Built for one job: copying the stock TS-01 camera firmware (camera/0.1.7.09) off a still-stock
"donor" camera without opening it. The donor's own stock updater installs this firmware into its
inactive slot and leaves the stock image in the other one; this component then serves that slot:

  GET  /dump/info        JSON (partition table, running slot, image length, on-device SHA-256)
  GET  /dump/app         raw bytes, trimmed to the image length (?trim=0: the whole slot)
  POST /dump/ota-unlock  with ota_id only: allow OTA once the slot hashes to expected_sha256

All behind HTTP Basic auth (user `dumper`). It never writes flash: see flash_dumper.cpp (the write,
erase and boot-switch calls are poisoned) and esphome/tests/test_flash_dumper.cpp.
Procedure: docs/camera-donor-dump.md. Verifier: tools/verify_camera_dump.py.
"""
import logging
import re
import secrets
from pathlib import Path

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import esp32
from esphome.components.esphome.ota import ESPHomeOTAComponent
from esphome.const import CONF_ID, CONF_PASSWORD, CONF_PORT, CONF_USERNAME
from esphome.core import CORE

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@aishroom"]
DEPENDENCIES = ["network", "logger", "wifi"]
AUTO_LOAD = ["json"]

CONF_EXPECTED_SHA256 = "expected_sha256"
CONF_OTA_ID = "ota_id"
CONF_CHAMBER_LINK_VERSION = "chamber_link_version"

# SHA-256 of the stock camera image camera/0.1.7.09, as the stock main firmware demands it in its own
# camera update request (analysis/firmware_findings.md, "Camera-board protocol").
STOCK_CAMERA_SHA256 = "127772ee59465bd48df0588d2effdbc7299818891c583f432707d17f32ba5903"

# Placeholder-looking passwords that must never be the live one.
_WEAK = re.compile(r"^(password|changeme|dumper|admin|secret|test|example|placeholder)", re.I)

flash_dumper_ns = cg.esphome_ns.namespace("flash_dumper")
FlashDumper = flash_dumper_ns.class_("FlashDumper", cg.Component)


def _password(value):
    value = cv.string_strict(value)
    if len(value) < 16:
        raise cv.Invalid("use at least 16 characters (openssl rand -hex 16)")
    if _WEAK.match(value):
        raise cv.Invalid("that looks like a placeholder; generate a fresh one per build (openssl rand -hex 16)")
    return value


def _sha256(value):
    value = cv.string_strict(value).lower()
    if not re.fullmatch(r"[0-9a-f]{64}", value):
        raise cv.Invalid("64 hex digits")
    return value


def _version(value):
    value = cv.string_strict(value)
    # The chamber's camera install accepts letters, digits, '.', '_' and '-', 24 characters at most.
    if not re.fullmatch(r"[A-Za-z0-9._-]{1,24}", value):
        raise cv.Invalid("1-24 characters from A-Z a-z 0-9 . _ -")
    return value


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(FlashDumper),
        cv.Optional(CONF_PORT, default=80): cv.port,
        cv.Optional(CONF_USERNAME, default="dumper"): cv.All(cv.string_strict, cv.Length(min=1, max=32)),
        # Omitted: a random one is generated for this build, logged once and written next to the build.
        cv.Optional(CONF_PASSWORD): _password,
        cv.Optional(CONF_EXPECTED_SHA256, default=STOCK_CAMERA_SHA256): _sha256,
        # The build's `ota: platform: esphome` (it needs `password:`, empty is fine). Locked with a random
        # password at boot; POST /dump/ota-unlock swaps in this component's password once the dump
        # verifies. Upload then with: esphome -s ota_upload_password <password> upload ...
        cv.Optional(CONF_OTA_ID): cv.use_id(ESPHomeOTAComponent),
        # Get Wi-Fi from the chamber over the camera link, reporting automyco-camera/<this>.
        cv.Optional(CONF_CHAMBER_LINK_VERSION): _version,
    }
).extend(cv.COMPONENT_SCHEMA)


def _final_validate(config):
    if CONF_OTA_ID not in config:
        return
    full = fv.full_config.get()
    for ota_conf in full.get("ota") or []:
        if ota_conf.get(CONF_ID) == config[CONF_OTA_ID]:
            # `password:` must be present (it compiles in the auth check that the lock relies on). Leave it
            # empty in the build: this component locks OTA at boot and unlocks it with its own password.
            if CONF_PASSWORD not in ota_conf:
                raise cv.Invalid("flash_dumper ota_id: the ota block needs `password:` (empty is fine)")
            if ota_conf[CONF_PASSWORD] and CONF_PASSWORD in config and ota_conf[CONF_PASSWORD] != config[CONF_PASSWORD]:
                raise cv.Invalid("flash_dumper ota_id: the ota password differs from flash_dumper's")
            return
    raise cv.Invalid("flash_dumper ota_id: no ota block with that id")


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_port(config[CONF_PORT]))

    password = config.get(CONF_PASSWORD)
    if password is None:
        password = secrets.token_hex(16)
        out = Path(CORE.build_path) / "flash_dumper_password.txt"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(f"{config[CONF_USERNAME]}:{password}\n")
        _LOGGER.warning("flash_dumper: random login for THIS build is %s:%s (also in %s)",
                        config[CONF_USERNAME], password, out)
    cg.add(var.set_credentials(config[CONF_USERNAME], password))
    cg.add(var.set_expected_sha256(config[CONF_EXPECTED_SHA256]))

    if CONF_OTA_ID in config:
        ota = await cg.get_variable(config[CONF_OTA_ID])
        cg.add(var.set_ota(ota))
    if CONF_CHAMBER_LINK_VERSION in config:
        cg.add(var.set_link_version(config[CONF_CHAMBER_LINK_VERSION]))

    esp32.include_builtin_idf_component("esp_http_server")
