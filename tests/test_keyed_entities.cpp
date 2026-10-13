// Host tests for the public chamber's keyed-entity table (terrashroom-public.yaml web_guard keyed_entities)
// through control_key.h keyed_request: the unsafe direction of each safety control needs X-AutoMyco-Key once a
// phone has paired, the safe direction (open_action) and every read stay open, in every URL spelling ESPHome
// web_server accepts. tools/test_keyed_contract.py checks this table matches the YAML, the safety checker,
// the iOS mock and the names the app uses.
//   cd esphome && g++ -std=c++17 -Wall -Wextra -Werror -I. -Icommon tests/test_keyed_entities.cpp -o /tmp/t && /tmp/t
#include "components/web_guard/control_key.h"

#include <cstdio>

using namespace esphome::web_guard;

static int failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
      failures++;                                                            \
    }                                                                        \
  } while (0)

// As web_guard/__init__.py registers them: (name.lower(), sanitize(snake_case(name)).lower(), open_action).
// KEYED-TABLE-BEGIN
static const std::vector<KeyedEntity> kTable = {
    {"uv-c lamp", "uv-c_lamp", "turn_off"},
    {"camera reset login", "camera_reset_login", ""},
    {"restart", "restart", ""},
    {"camera power cycle", "camera_power_cycle", ""},
    {"remote pairing code", "remote_pairing_code", ""},
    {"water lockout override", "water_lockout_override", "turn_off"},
    {"water sensing", "water_sensing", ""},
    {"atomizer hard stop", "atomizer_hard_stop", "turn_on"},
    {"water probe drive", "water_probe_drive", ""},
    {"manual mist limit", "manual_mist_limit", ""},
    {"install firmware update", "install_firmware_update", ""},
    {"auto-install updates", "auto-install_updates", ""},
    {"camera install", "camera_install", ""},
};
// KEYED-TABLE-END

// The path as web_server sees it (url_to decodes once), from the request line the app sends.
static bool keyed(const std::string &request_target) {
  const std::string path = request_target.substr(0, request_target.find('?'));
  return keyed_request(pct_decode(path), kTable);
}

static void test_unsafe_directions_keyed() {
  // The URLs the iOS app builds (DeviceCommand.relativeURLString: percent-encoded Name, POST).
  for (const char *t : {"/switch/UV-C%20Lamp/turn_on", "/switch/UV-C%20Lamp/toggle", "/button/Camera%20Reset%20Login/press",
                        "/button/Restart/press", "/button/Camera%20Power%20Cycle/press",
                        "/text/Remote%20Pairing%20Code/set?value=ABCD2345",
                        "/switch/Water%20Lockout%20Override/turn_on", "/switch/Water%20Lockout%20Override/toggle",
                        "/select/Water%20Sensing/set?option=Off", "/switch/Atomizer%20Hard%20Stop/turn_off",
                        "/switch/Atomizer%20Hard%20Stop/toggle", "/select/Water%20Probe%20Drive/set?option=Steady",
                        "/number/Manual%20Mist%20Limit/set?value=120", "/button/Install%20Firmware%20Update/press",
                        "/switch/Auto-Install%20Updates/turn_on", "/switch/Auto-Install%20Updates/turn_off",
                        "/text/Camera%20Install/set?value=1.2.0%20abcd"}) {
    if (!keyed(t))
      std::printf("  not keyed: %s\n", t);
    CHECK(keyed(t));
  }
}

static void test_other_spellings_keyed() {
  // Legacy object_id URLs, domain-prefixed ids, odd case, double encoding, extra segments.
  for (const char *t : {"/button/restart/press", "/switch/water_lockout_override/turn_on", "/select/water_sensing/set",
                        "/switch/switch-atomizer_hard_stop/turn_off", "/BUTTON/RESTART/PRESS", "/button/Restart/press/",
                        "/text/Remote%2520Pairing%2520Code/set", "/x/../button/Camera%20Power%20Cycle/press",
                        "/number/manual_mist_limit/set", "/switch/auto-install_updates/turn_on"}) {
    if (!keyed(t))
      std::printf("  not keyed: %s\n", t);
    CHECK(keyed(t));
  }
}

static void test_safe_directions_and_others_open() {
  for (const char *t : {"/switch/UV-C%20Lamp/turn_off", "/switch/Water%20Lockout%20Override/turn_off",
                        "/switch/Atomizer%20Hard%20Stop/turn_on", "/switch/Heater/turn_on", "/switch/Fan%201/turn_on",
                        "/number/Humidity%20Setpoint/set?value=88", "/button/Start%20Grow/press",
                        "/button/Check%20for%20Firmware%20Update/press", "/switch/Check%20for%20Updates/turn_off",
                        "/text/Update%20Pairing/set?value=x", "/button/Reset%20Cloud%20Identity/press",
                        "/button/Open%20Adoption%20Window/press", "/switch/Remote%20Access/turn_off",
                        "/switch/Camera%20Power/turn_on"}) {
    if (keyed(t))
      std::printf("  keyed but shouldn't be: %s\n", t);
    CHECK(!keyed(t));
  }
}

int main() {
  test_unsafe_directions_keyed();
  test_other_spellings_keyed();
  test_safe_directions_and_others_open();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("keyed_entities: all checks passed\n");
  return 0;
}
