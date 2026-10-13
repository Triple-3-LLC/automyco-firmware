"""fw_signing: signed app images, checked on update, with NO secure boot and NO eFuses (issue #32).

A chamber running a build with this component only accepts an OTA image (ESPHome native OTA,
`ota.http_request` fleet updates, the iOS app) that carries a valid ECDSA P-256 signature from the
AutoMyco firmware key. ESP-IDF checks it in esp_ota_end() (and again in esp_ota_set_boot_partition),
before the new slot is marked bootable, so a rejected image never runs and the running firmware
is untouched.

What the build needs (ESP-IDF 5.5 Kconfig, components/bootloader/Kconfig.projbuild). The YAML sets
the switches itself in `esp32: framework: sdkconfig_options`, where a reviewer sees them; this
component refuses to build unless every one is there with exactly this value, then adds the key:

  CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y      require signed apps, without hardware secure boot
  CONFIG_SECURE_SIGNED_APPS_ECDSA_SCHEME=y        Secure Boot V1 signature format (ECDSA P-256):
                                                  works on every ESP32 revision (RSA/V2 needs rev 3)
  CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=y check the signature on every OTA update
  CONFIG_SECURE_SIGNED_ON_BOOT_NO_SECURE_BOOT=n   NOT on boot: that needs a new bootloader, which no
                                                  OTA can install
  CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n      the build never sees the private key; the release
                                                  script signs (site/scripts/publish_firmware.py)
  CONFIG_SECURE_BOOT=n, CONFIG_SECURE_FLASH_ENC_ENABLED=n
                                                  hardware secure boot and flash encryption stay off
  CONFIG_SECURE_BOOT_VERIFICATION_KEY=<path>      (added here) the raw 64-byte public key compiled
                                                  into the app

Nothing here can write an eFuse: the CLAUDE.md hard rule. A chamber can always be recovered by a
serial reflash.

The key: `public_key:` (default signing/fw_pub.pem, relative to the YAML) is the production public
key. If the default file is missing, the build falls back to the TEST key in signing/test/ and says
so loudly. The test key's private half was destroyed when it was made, so a test-key build can only
ever be updated over serial, and publish_firmware.py refuses to publish one. CI builds with it.

This component writes no C++: it only checks and adds sdkconfig options, so it costs the image
nothing beyond ESP-IDF's own verifier (mbedTLS ECDSA) and the 64-byte key.
"""

import base64
import binascii
import logging
from pathlib import Path

import esphome.config_validation as cv
from esphome.components.esp32 import add_idf_sdkconfig_option, get_esp32_variant
from esphome.components.esp32.const import VARIANT_ESP32
from esphome.core import CORE
import esphome.final_validate as fv

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@triple-3-llc"]

CONF_PUBLIC_KEY = "public_key"
DEFAULT_KEY = "signing/fw_pub.pem"
TEST_KEY = "signing/test/fw_test_pub.pem"
KEY_BIN = "fw_signing_verification_key.bin"   # written into the build dir, raw X||Y

# DER SubjectPublicKeyInfo prefix of an uncompressed P-256 public key (id-ecPublicKey, prime256v1),
# up to and including the 0x04 "uncompressed point" byte. X and Y (32 bytes each, big-endian) follow.
P256_SPKI_PREFIX = bytes.fromhex("3059301306072a8648ce3d020106082a8648ce3d030107034200") + b"\x04"

# Required `sdkconfig_options`, exactly. CONFIG_SECURE_BOOT and CONFIG_SECURE_FLASH_ENC_ENABLED are off
# by default anyway; requiring them spelled out as "n" means a future default change can't quietly
# turn on a path that burns eFuses.
REQUIRED_SDKCONFIG = {
    "CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT": "y",
    "CONFIG_SECURE_SIGNED_APPS_ECDSA_SCHEME": "y",
    "CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT": "y",
    "CONFIG_SECURE_SIGNED_ON_BOOT_NO_SECURE_BOOT": "n",
    "CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES": "n",
    "CONFIG_SECURE_BOOT": "n",
    "CONFIG_SECURE_FLASH_ENC_ENABLED": "n",
}
# Never allowed next to fw_signing, whatever the value: each is a step towards eFuse writes or a
# bootloader the field can't receive.
FORBIDDEN_SDKCONFIG = (
    "CONFIG_SECURE_BOOT_V1_ENABLED", "CONFIG_SECURE_BOOT_V2_ENABLED", "CONFIG_SECURE_BOOT_SIGNING_KEY",
    "CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT", "CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE",
    "CONFIG_SECURE_BOOT_VERIFICATION_KEY",   # fw_signing sets it from public_key:
)


def check_sdkconfig(options):
    """Return a list of problems with a `sdkconfig_options` dict (empty = fine)."""
    problems = [f'{k}: "{v}" (found {options.get(k)!r})' for k, v in REQUIRED_SDKCONFIG.items() if options.get(k) != v]
    problems += [f"{k} must not be set" for k in FORBIDDEN_SDKCONFIG if k in options]
    return problems


def raw_p256_public_key(pem: str) -> bytes:
    """The 64-byte X||Y form ESP-IDF embeds (what `espsecure extract-public-key --version 1` writes)."""
    if "-----BEGIN PUBLIC KEY-----" not in pem:
        raise ValueError("not a PEM public key (expected -----BEGIN PUBLIC KEY-----)")
    body = "".join(line.strip() for line in pem.splitlines() if line.strip() and not line.startswith("-----"))
    try:
        der = base64.b64decode(body, validate=True)
    except (binascii.Error, ValueError) as err:
        raise ValueError(f"bad base64 in PEM: {err}") from err
    if len(der) != len(P256_SPKI_PREFIX) + 64 or not der.startswith(P256_SPKI_PREFIX):
        raise ValueError("not an uncompressed ECDSA P-256 (prime256v1) public key; Secure Boot V1 needs exactly that")
    return der[len(P256_SPKI_PREFIX):]


def _resolve_key(config):
    explicit = bool(config.get(CONF_PUBLIC_KEY))   # "" (the YAML's default substitution) = not given
    path = Path(CORE.relative_config_path(config.get(CONF_PUBLIC_KEY) or DEFAULT_KEY))
    test = Path(CORE.relative_config_path(TEST_KEY))
    if not path.is_file():
        if explicit:
            raise cv.Invalid(f"fw_signing: public key {path} does not exist")
        path = test
        bar = "!" * 78
        _LOGGER.warning(
            "\n%s\n fw_signing: %s is missing, so this build embeds the TEST verification key\n"
            " (%s). Its private half no longer exists: a chamber running this image accepts\n"
            " NO over-the-air update, only a serial reflash. publish_firmware.py refuses to\n"
            " publish it. Fine for CI; for a release, run the key ceremony (tools/firmware_signing_key.py).\n%s",
            bar, DEFAULT_KEY, TEST_KEY, bar)
    try:
        raw = raw_p256_public_key(path.read_text())
    except ValueError as err:
        raise cv.Invalid(f"fw_signing: {path}: {err}") from err
    config["_vk_path"] = str(path)
    config["_vk_raw"] = raw.hex()
    config["_vk_is_test"] = path.resolve() == test.resolve()
    return config


def _only_esp32_classic(config):
    # Secure Boot V1 signatures exist only on the original ESP32 (the TS-01's WROOM-32).
    if get_esp32_variant() != VARIANT_ESP32:
        raise cv.Invalid("fw_signing uses the Secure Boot V1 (ECDSA) scheme, which only the original ESP32 has")
    return config


def _final_validate(config):
    options = fv.full_config.get()["esp32"]["framework"].get("sdkconfig_options", {})
    problems = check_sdkconfig(options)
    if problems:
        raise cv.Invalid("fw_signing needs exactly these in esp32: framework: sdkconfig_options:\n  " + "\n  ".join(problems))
    return config


FINAL_VALIDATE_SCHEMA = _final_validate

CONFIG_SCHEMA = cv.All(
    cv.Schema({cv.Optional(CONF_PUBLIC_KEY): cv.string_strict}, extra=cv.ALLOW_EXTRA),
    cv.only_on_esp32,
    _only_esp32_classic,
    _resolve_key,
)


async def to_code(config):
    # ESP-IDF resolves a relative key path against the PlatformIO project dir (the build dir), so
    # hand it an absolute path to a copy inside the build dir: no dependence on where the YAML lives.
    raw = bytes.fromhex(config["_vk_raw"])
    out = Path(CORE.relative_build_path(KEY_BIN))
    out.parent.mkdir(parents=True, exist_ok=True)
    if not out.is_file() or out.read_bytes() != raw:
        out.write_bytes(raw)
    add_idf_sdkconfig_option("CONFIG_SECURE_BOOT_VERIFICATION_KEY", str(out.resolve()))
    _LOGGER.info("fw_signing: OTA images must be signed by the key in %s%s", config["_vk_path"],
                 " (TEST key)" if config["_vk_is_test"] else "")
