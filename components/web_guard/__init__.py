"""web_guard: CSRF / DNS-rebinding guard in front of ESPHome's web_server (ESP32, ESP-IDF server).

ESPHome 2026.9's web_server answers every response with `Access-Control-Allow-Origin: *`, runs
entity actions on GET (`GET /switch/UV-C/turn_on`), does not check the Origin of the SSE stream and
has no Host check, so a web page in any browser on the LAN could read or drive the node. This
component is registered as the first request handler and refuses, before web_server or its auth
runs:

  * a Host that is not this node (IP literal, <node name>, <node name>.local, or allowed_hosts)
  * every OPTIONS request (no CORS preflight is ever granted)
  * any request whose Origin is not exactly http://<Host>
  * GET requests that name an action (/<domain>/<name>/<action>); actions are POST-only
  * POSTs that carry neither a same-origin Origin nor an X-Requested-With header
  * state changes to `keyed_entities` without a valid X-AutoMyco-Key (HMAC over a single-use challenge,
    control_key.h),
    once the firmware has set an owner password (the AutoMyco public build: the paired OTA password)

It also swaps the wildcard Access-Control-Allow-Origin default header for X-Content-Type-Options.
Authentication, where a device has it, stays with web_server's `auth:`. With `require_auth: true`
(the default) the build refuses a missing, short or placeholder password. Devices whose LAN API is
deliberately unauthenticated (the TS-01 builds) set `require_auth: false`: the guard still stops
cross-site and DNS-rebinding requests, because a browser can't add X-Requested-With cross-site
without a preflight, and preflights are refused.

Shared by AutoMyco (TS-01) and Phytonode; keep one copy. It reaches into ESPHome internals
(web_server_idf::DefaultHeaders), so re-check it after every ESPHome upgrade. Threat model and curl
checks: docs/device-api.md "Security".
"""

import esphome.codegen as cg
from esphome.components import text_sensor, web_server_base
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
import esphome.config_validation as cv
from esphome.const import CONF_AUTH, CONF_ID, CONF_NAME, CONF_PASSWORD, PLATFORM_ESP32
from esphome.helpers import sanitize, snake_case
import esphome.final_validate as fv

CODEOWNERS = ["@triple-3-llc"]
DEPENDENCIES = ["web_server_base", "web_server"]

CONF_ALLOWED_HOSTS = "allowed_hosts"
CONF_REQUIRE_AUTH = "require_auth"
CONF_KEYED_ENTITIES = "keyed_entities"
CONF_OPEN_ACTION = "open_action"
CONF_CHALLENGE_SENSOR = "challenge_sensor"
MIN_PASSWORD_LENGTH = 12
# Values that ship in secrets.example.yaml or docs; a node must never run with one of them.
PLACEHOLDER_PASSWORDS = {"change-me-node-web", "change-me", "password", "phytonode", "automyco", "terrashroom"}

web_guard_ns = cg.esphome_ns.namespace("web_guard")
WebGuard = web_guard_ns.class_("WebGuard", cg.Component)


def _hostname(value):
    value = cv.string_strict(value).strip().lower().rstrip(".")
    if not value or any(c in value for c in "/: @"):
        raise cv.Invalid(f"'{value}' is not a bare host name (no scheme, port or path)")
    return value


KEYED_ENTITY_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_NAME): cv.string_strict,
        # The one action that needs no key (UV-C: turn_off). Every other state change needs it.
        cv.Optional(CONF_OPEN_ACTION, default=""): cv.string_strict,
    }
)


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(WebGuard),
            cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(web_server_base.WebServerBase),
            cv.Optional(CONF_ALLOWED_HOSTS, default=[]): cv.ensure_list(_hostname),
            cv.Optional(CONF_REQUIRE_AUTH, default=True): cv.boolean,
            # Entities (by name) whose state changes also need X-AutoMyco-Key once the firmware sets an
            # owner password (set_owner_password). Empty: nothing keyed (personal build, Phytonode).
            cv.Optional(CONF_KEYED_ENTITIES, default=[]): cv.ensure_list(KEYED_ENTITY_SCHEMA),
            # A template text_sensor the guard publishes its single-use challenge to (Control Challenge).
            cv.Optional(CONF_CHALLENGE_SENSOR): cv.use_id(text_sensor.TextSensor),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on([PLATFORM_ESP32]),
)


def _entity_names(node, out):
    if isinstance(node, dict):
        name = node.get(CONF_NAME)
        if isinstance(name, str):
            out.add(name)
        for value in node.values():
            _entity_names(value, out)
    elif isinstance(node, list):
        for value in node:
            _entity_names(value, out)
    return out


def _final_validate(config):
    full = fv.full_config.get()
    if config[CONF_KEYED_ENTITIES]:
        # A renamed entity must not silently drop out of the gate.
        names = _entity_names({k: v for k, v in full.items() if k != "web_guard"}, set())
        for entity in config[CONF_KEYED_ENTITIES]:
            if entity[CONF_NAME] not in names:
                raise cv.Invalid(f"web_guard keyed_entities: no entity named '{entity[CONF_NAME]}'")
    web = full.get("web_server")
    if web is None:
        return config
    if web.get("enable_private_network_access") or web.get("allowed_origins"):
        raise cv.Invalid(
            "web_guard refuses every cross-origin request; remove web_server "
            "allowed_origins / enable_private_network_access"
        )
    auth = web.get(CONF_AUTH)
    if auth is None and not config[CONF_REQUIRE_AUTH]:
        return config
    if auth is None:
        raise cv.Invalid(
            "web_guard needs web_server `auth:` (a per-node password); it only stops cross-site "
            "requests, it does not authenticate the app"
        )
    password = str(auth[CONF_PASSWORD])
    if len(password) < MIN_PASSWORD_LENGTH or password.lower() in PLACEHOLDER_PASSWORDS:
        raise cv.Invalid(
            f"web_server auth password must be at least {MIN_PASSWORD_LENGTH} characters and not "
            "a placeholder; generate one per node (see secrets.example.yaml)"
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    base = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    var = cg.new_Pvariable(config[CONF_ID], base)
    await cg.register_component(var, config)
    for host in config[CONF_ALLOWED_HOSTS]:
        cg.add(var.add_allowed_host(host))
    if CONF_CHALLENGE_SENSOR in config:
        sens = await cg.get_variable(config[CONF_CHALLENGE_SENSOR])
        cg.add(var.set_challenge_sensor(sens))
    for entity in config[CONF_KEYED_ENTITIES]:
        name = entity[CONF_NAME]
        # Matched case-insensitively, by name and by object_id (the legacy URL form), see control_key.h.
        cg.add(var.add_keyed_entity(name.lower(), sanitize(snake_case(name)).lower(), entity[CONF_OPEN_ACTION]))
