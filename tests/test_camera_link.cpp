// Host tests for the camera install state machine (esphome/components/camera_link/camera_link_machine.h).
//   cd esphome && g++ -std=c++17 -Wall -Wextra -I. -Icommon tests/test_camera_link.cpp -o /tmp/tcl && /tmp/tcl
// The camera lines are the real ones from the 2026-10-09 bench log (LOG.md): the stock camera's
// updater output, its DNS failures on bad Wi-Fi, and the successful install.
#include "components/camera_link/camera_link_machine.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace camera_install;

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      failures++;                                                       \
    }                                                                   \
  } while (0)

static const std::string HASH = "33f51615f07a821b8d7f7ec8c972dfcaa85d682e2068df033732c2c5f73e8222";
static const std::string STOCK_READY = "{\"status\":\"ready\",\"version\":\"camera/0.1.7.09\"}";
static const std::string NEW_READY = "{\"status\":\"ready\",\"version\":\"automyco-camera/1.0.0\"}";

struct Bench {
  uint32_t now = 0;
  bool power = false;
  bool chamber_wifi = true;
  int power_offs = 0, wifi_sends = 0;
  std::vector<std::string> sent, statuses, events;
  std::string firmware, address;
  InstallMachine m{Io{
      [this](bool on) { power = on; if (!on) power_offs++; },
      [this](const std::string &l) { sent.push_back(l); },
      [this]() { return chamber_wifi; },
      [this]() { if (!chamber_wifi) return false; wifi_sends++; return true; },
      [this](const std::string &s) { statuses.push_back(s); },
      [this](const std::string &v) { firmware = v; },
      [this](const std::string &a) { address = a; },
      [this](const std::string &e) { events.push_back(e); },
  }};

  void advance(uint32_t ms) {  // the main loop runs every ~16 ms
    for (uint32_t t = 0; t < ms; t += 16) { now += 16; m.tick(now); }
  }
  void line(const std::string &l) { m.line(l, now); }
  std::string status() const { return statuses.empty() ? "" : statuses.back(); }
  bool sent_ota() const {
    for (auto &l : sent) if (l.find("{\"ota\":") == 0) return true;
    return false;
  }
  // Power-cycle -> stock ready -> Wi-Fi -> connected -> ota command.
  void to_requested() {
    advance(InstallMachine::POWER_CYCLE_OFF_MS + 100);
    CHECK(power);
    line(STOCK_READY);
    CHECK(status() == "sending Wi-Fi");
    advance(2100);
    line("Connected to WiFi");
    advance(2100);
  }
  void stock_attempt_dns_failure() {
    line("[ 7010][D][HTTPClient.cpp:303] beginInternal(): protocol: https, host: api.terrashroom.org port: 443 url: /firmware_url");
    advance(7000);
    line("[ 14035][E][WiFiGeneric.cpp:1583] hostByName(): DNS Failed for api.terrashroom.org");
    line("Got signed URL: ");
    line("Update failed, attempt 1 of 3");
    advance(5000);
  }
};

static void test_happy_path() {
  std::printf("happy path\n");
  Bench b;
  b.m.start("1.0.0 " + HASH, b.now);
  CHECK(b.status() == "starting camera");
  CHECK(!b.power && b.power_offs == 1);
  b.to_requested();
  CHECK(b.wifi_sends == 1);
  CHECK(b.sent_ota());
  CHECK(b.sent.back() == "{\"ota\":{\"version\":\"automyco-camera/1.0.0\",\"hash\":\"" + HASH + "\"}}");
  CHECK(b.status() == "requesting firmware");
  b.line("=> {\"ota\":{\"version\":\"automyco-camera/1.0.0\",\"hash\":\"" + HASH + "\"}}");  // stock echo
  b.line("[ 42519][D][HTTPClient.cpp:1321] handleHeaderResponse(): code: 200");
  b.line("Got signed URL: https://api.terrashroom.org/fw/" + HASH + ".bin");
  b.line("[1934047][D][Updater.cpp:139] begin(): OTA Partition: app1");
  b.line("Starting OTA update...");
  CHECK(b.status() == "downloading");
  b.advance(60000);
  b.line("Update successfully completed. Rebooting...");
  CHECK(b.status() == "restarting camera");
  b.advance(3000);
  b.line(NEW_READY);
  CHECK(b.firmware == "automyco-camera/1.0.0");
  CHECK(b.wifi_sends == 2);
  b.line("{\"status\":\"online\",\"ip\":\"192.0.2.21\"}");
  CHECK(b.status() == "installed");
  CHECK(b.address == "192.0.2.21");
  CHECK(b.m.step() == InstallMachine::Step::IDLE);
}

static void test_flaky_wifi_retries_then_succeeds() {
  std::printf("flaky stock Wi-Fi: round 1 fails, round 2 installs\n");
  Bench b;
  b.m.start("1.0.0 " + HASH, b.now);
  b.to_requested();
  for (int i = 0; i < 3; i++) b.stock_attempt_dns_failure();
  b.line("Maximum update attempts reached. OTA aborted.");
  CHECK(b.m.round() == 2);
  CHECK(b.power_offs == 2);  // power-cycled for the second round
  CHECK(b.status() == "starting camera");
  b.to_requested();
  b.line("Starting OTA update...");
  b.line("Update successfully completed. Rebooting...");
  b.line(NEW_READY);
  b.line("{\"status\":\"online\",\"ip\":\"10.0.0.5\"}");
  CHECK(b.status() == "installed");
}

static void test_dns_failure_after_a_200_is_not_a_refusal() {
  std::printf("a DNS failure after an earlier 200 is not read as \"not offered\"\n");
  Bench b;
  b.m.start("1.0.0 " + HASH, b.now);
  b.to_requested();
  b.line("[1] beginInternal(): protocol: https, host: api.terrashroom.org port: 443 url: /firmware_url");
  b.line("handleHeaderResponse(): code: 200");
  b.line("Got signed URL: https://api.terrashroom.org/fw/x.bin");
  b.line("handleHeaderResponse(): code: 404");  // S3/fw GET fails
  b.line("[2] beginInternal(): protocol: https, host: api.terrashroom.org port: 443 url: /firmware_url");
  b.line("hostByName(): DNS Failed for api.terrashroom.org");
  b.line("Got signed URL: ");
  CHECK(b.m.step() == InstallMachine::Step::REQUESTED);
  CHECK(b.status() == "requesting firmware");
}

static void test_server_refuses() {
  std::printf("server returns an empty URL -> failed: firmware not offered\n");
  Bench b;
  b.m.start("1.0.0 " + HASH, b.now);
  b.to_requested();
  b.line("beginInternal(): protocol: https, host: api.terrashroom.org port: 443 url: /firmware_url");
  b.line("handleHeaderResponse(): code: 200");
  b.line("Got signed URL: ");
  CHECK(b.status() == "failed: firmware not offered");
  CHECK(b.m.step() == InstallMachine::Step::IDLE);
}

static void test_gives_up_after_four_rounds() {
  std::printf("download keeps failing -> four rounds, then failed\n");
  Bench b;
  b.m.start("1.0.0 " + HASH, b.now);
  for (int r = 0; r < 4; r++) {
    b.to_requested();
    b.line("Maximum update attempts reached. OTA aborted.");
  }
  CHECK(b.status() == "failed: download kept failing");
  CHECK(b.power_offs == 4);
}

static void test_camera_silent() {
  std::printf("no ready line -> camera not responding after four rounds\n");
  Bench b;
  b.m.start("1.0.0 " + HASH, b.now);
  b.advance(4 * (InstallMachine::POWER_CYCLE_OFF_MS + 21000));
  CHECK(b.status() == "failed: camera not responding");
  CHECK(!b.sent_ota());
}

static void test_no_chamber_wifi() {
  std::printf("chamber not on Wi-Fi -> failed, nothing sent\n");
  Bench b;
  b.chamber_wifi = false;
  b.m.start("1.0.0 " + HASH, b.now);
  b.advance(InstallMachine::POWER_CYCLE_OFF_MS + 100);
  b.line(STOCK_READY);
  CHECK(b.status() == "failed: no Wi-Fi to give the camera");
  CHECK(b.sent.empty());
}

static void test_already_installed_and_other_automyco() {
  std::printf("camera already on the target / another AutoMyco version\n");
  Bench a;
  a.m.start("1.0.0 " + HASH, a.now);
  a.advance(InstallMachine::POWER_CYCLE_OFF_MS + 100);
  a.line(NEW_READY);
  CHECK(a.status() == "already installed");
  CHECK(!a.sent_ota());
  // Another AutoMyco version (camera_update): it gets Wi-Fi and the ota command, no stock steps.
  Bench b;
  b.m.start("1.1.0 " + HASH, b.now);
  b.advance(InstallMachine::POWER_CYCLE_OFF_MS + 100);
  b.line(NEW_READY);
  CHECK(b.wifi_sends == 1);
  CHECK(b.sent_ota());
  CHECK(b.sent.back() == "{\"ota\":{\"version\":\"automyco-camera/1.1.0\",\"hash\":\"" + HASH + "\"}}");
  CHECK(b.status() == "requesting firmware");
}

static const std::string READY_110 = "{\"status\":\"ready\",\"version\":\"automyco-camera/1.1.0\"}";

static void test_automyco_camera_updates_without_power_cycle() {
  std::printf("AutoMyco camera already up: update with no power cycle\n");
  Bench b;
  b.power = true;
  b.line(NEW_READY);  // at boot, or the chamber's status query
  CHECK(b.wifi_sends == 1);
  b.m.start("1.1.0 " + HASH, b.now);
  CHECK(b.power_offs == 0);
  CHECK(b.sent_ota());
  CHECK(b.status() == "requesting firmware");
  b.line("{\"status\":\"ota\",\"state\":\"waiting\",\"version\":\"1.1.0\"}");
  b.line("Starting OTA update");
  CHECK(b.status() == "downloading");
  b.line("{\"status\":\"ota\",\"state\":\"downloading\",\"progress\":50,\"version\":\"1.1.0\"}");
  b.advance(30000);
  b.line("Update successfully completed. Rebooting...");
  CHECK(b.status() == "restarting camera");
  b.advance(4000);
  b.line(READY_110);
  CHECK(b.firmware == "automyco-camera/1.1.0");
  b.line("{\"status\":\"online\",\"ip\":\"192.168.1.23\"}");
  CHECK(b.status() == "installed");
  CHECK(b.power_offs == 0);
}

static void test_automyco_camera_already_on_target() {
  std::printf("AutoMyco camera already on the requested version: nothing sent\n");
  Bench b;
  b.power = true;
  b.line(READY_110);
  b.m.start("1.1.0 " + HASH, b.now);
  CHECK(b.status() == "already installed");
  CHECK(!b.sent_ota());
  CHECK(b.power_offs == 0);
}

static void test_automyco_camera_refuses() {
  std::printf("AutoMyco camera refuses (older, not published) -> failed with its reason, no retry rounds\n");
  Bench b;
  b.power = true;
  b.line(READY_110);
  b.m.start("1.0.0 " + HASH, b.now);
  CHECK(b.sent_ota());
  b.line("{\"status\":\"ota\",\"state\":\"failed\",\"version\":\"1.0.0\",\"error\":\"older than 1.1.0\"}");
  CHECK(b.status() == "failed: older than 1.1.0");
  CHECK(b.m.step() == InstallMachine::Step::IDLE);
  CHECK(b.power_offs == 0);
  // A failure line outside an install changes nothing.
  Bench c;
  c.line("{\"status\":\"ota\",\"state\":\"failed\",\"error\":\"x\"}");
  CHECK(c.statuses.empty());
  // Control characters in the reason never reach the status.
  Bench d;
  d.power = true;
  d.line(NEW_READY);
  d.m.start("1.1.0 " + HASH, d.now);
  d.line("{\"status\":\"ota\",\"state\":\"failed\",\"error\":\"release\u0007 not found\"}");
  CHECK(d.status().find('\x07') == std::string::npos);
  CHECK(d.status().rfind("failed: ", 0) == 0);
}

static void test_automyco_camera_silent_falls_back_to_rounds() {
  std::printf("AutoMyco camera doesn't answer the ota command -> power-cycle round, then it updates\n");
  Bench b;
  b.power = true;
  b.line(NEW_READY);
  b.m.start("1.1.0 " + HASH, b.now);
  CHECK(b.power_offs == 0);
  b.advance(120000 + 100);  // REQUESTED timed out
  CHECK(b.m.round() == 2);
  CHECK(b.power_offs == 1);
  b.advance(InstallMachine::POWER_CYCLE_OFF_MS + 100);
  CHECK(b.power);
  const size_t before = b.sent.size();
  b.line(NEW_READY);
  CHECK(b.sent.size() > before && b.sent.back().find("{\"ota\":") == 0);
  // Camera Power switched off by hand: the last ready line no longer counts.
  Bench c;
  c.power = true;
  c.line(NEW_READY);
  c.m.camera_off();
  c.m.start("1.1.0 " + HASH, c.now);
  CHECK(c.power_offs == 1);
  CHECK(c.status() == "starting camera");
}

static void test_rejected_image_boots_stock() {
  std::printf("camera reboots back to stock -> next round\n");
  Bench b;
  b.m.start("1.0.0 " + HASH, b.now);
  b.to_requested();
  b.line("Starting OTA update...");
  b.line(STOCK_READY);
  CHECK(b.m.round() == 2);
}

static void test_invalid_requests() {
  std::printf("invalid requests are refused before anything happens\n");
  for (const std::string &req : std::vector<std::string>{"", "1.0.0", "1.0.0 " + HASH.substr(1), "1.0.0 " + std::string(64, 'G'),
                                "1.0.0  " + HASH, "../x " + HASH, std::string(25, '1') + " " + HASH}) {
    Bench b;
    b.m.start(req, b.now);
    CHECK(b.status() == "failed: invalid request");
    CHECK(b.power_offs == 0);
  }
}

static void test_overall_timeout() {
  std::printf("15-minute overall timeout\n");
  Bench b;
  b.m.start("1.0.0 " + HASH, b.now);
  b.to_requested();
  b.line("Starting OTA update...");
  for (int i = 0; i < 4 && b.m.step() != InstallMachine::Step::IDLE; i++) {
    b.advance(4 * 60 * 1000);
    if (b.m.step() == InstallMachine::Step::WAIT_READY) b.line(STOCK_READY);
  }
  b.advance(16 * 60 * 1000);
  CHECK(b.m.step() == InstallMachine::Step::IDLE);
  CHECK(b.status().rfind("failed: ", 0) == 0);
}

static void test_idle_keeps_automyco_camera_on_wifi() {
  std::printf("outside an install, an AutoMyco camera's boot gets Wi-Fi; a stock one doesn't\n");
  Bench b;
  b.line(STOCK_READY);
  CHECK(b.wifi_sends == 0);
  CHECK(b.firmware == "camera/0.1.7.09");
  b.line(NEW_READY);
  CHECK(b.wifi_sends == 1);
  b.line("{\"status\":\"online\",\"ip\":\"192.168.1.9\"}");
  CHECK(b.address == "192.168.1.9");
  CHECK(b.statuses.empty());
}

static const std::string WEDGED = "{\"status\":\"power_cycle\",\"reason\":\"no valid camera frame for 60 s\"}";

static void test_watchdog_power_cycle() {
  std::printf("camera watchdog asks -> 10 s off, then on, one Activity entry\n");
  Bench b;
  b.power = true;
  b.line(WEDGED);
  CHECK(!b.power && b.power_offs == 1);
  CHECK(b.events.size() == 1 && b.events[0] == "camera power-cycled: camera: no valid camera frame for 60 s");
  b.advance(9000);
  CHECK(!b.power);
  b.advance(1100);
  CHECK(b.power);
  CHECK(b.statuses.empty());  // not an install
}

static void test_watchdog_backoff() {
  std::printf("repeated watchdog requests back off 5, 15, 60 min, then reset after 2 h quiet\n");
  Bench b;
  b.line(WEDGED);  // #1 runs
  b.advance(11000);
  b.line(WEDGED);  // too soon
  CHECK(b.power_offs == 1);
  b.advance(5 * 60 * 1000);
  b.line(WEDGED);  // #2 after 5 min
  CHECK(b.power_offs == 2);
  b.advance(10 * 60 * 1000);
  b.line(WEDGED);  // 10 min < 15
  CHECK(b.power_offs == 2);
  b.advance(6 * 60 * 1000);
  b.line(WEDGED);  // #3 after 16 min
  CHECK(b.power_offs == 3);
  b.advance(30 * 60 * 1000);
  b.line(WEDGED);  // 30 min < 60
  CHECK(b.power_offs == 3);
  b.advance(3 * 60 * 60 * 1000);
  b.line(WEDGED);  // quiet > 2 h: back to immediate
  CHECK(b.power_offs == 4);
}

static void test_reason_is_sanitised() {
  std::printf("the camera's reason is cleaned before it reaches the Event Log\n");
  Bench b;
  b.line("{\"status\":\"power_cycle\",\"reason\":\"<script>\u001b[31mx" + std::string(200, 'A') + "\"}");
  CHECK(b.events.size() == 1);
  CHECK(b.events[0].find('<') == std::string::npos && b.events[0].find('\\') == std::string::npos);
  CHECK(b.events[0].size() <= std::string("camera power-cycled: camera: ").size() + 60);
}

static void test_manual_cycle_and_install_guard() {
  std::printf("manual power cycle ignores backoff; nothing interrupts an install\n");
  Bench b;
  b.line(WEDGED);
  b.advance(11000);
  CHECK(b.m.power_cycle("Camera Power Cycle button", b.now, true));
  CHECK(b.power_offs == 2);
  b.advance(11000);
  CHECK(b.power);
  CHECK(!b.m.power_cycle("Camera Power Cycle button", b.now, true));  // once per 2 min
  CHECK(b.power_offs == 2);
  b.advance(2 * 60 * 1000);
  b.m.start("1.0.0 " + HASH, b.now);
  const int offs = b.power_offs;
  CHECK(!b.m.power_cycle("button", b.now, true));
  b.line(WEDGED);
  CHECK(b.power_offs == offs);
}

static void test_timeout_during_power_off_restores_power() {
  std::printf("overall timeout landing in a round's POWER_OFF switches the camera back on\n");
  Bench b;
  b.m.start("1.0.0 " + HASH, b.now);
  b.to_requested();
  b.line("Starting OTA update...");
  b.advance(5 * 60 * 1000 + 100);  // download timeout -> round 2 begins with the power off
  CHECK(b.m.round() == 2);
  CHECK(b.m.step() == InstallMachine::Step::POWER_OFF);
  CHECK(!b.power);
  // A long main-loop stall (an OTA write, say) jumps the clock past the 15-min limit mid-POWER_OFF.
  b.now += 16 * 60 * 1000;
  b.m.tick(b.now);
  CHECK(b.m.step() == InstallMachine::Step::IDLE);
  CHECK(b.status() == "failed: timed out");
  CHECK(b.power);
}

static void test_start_refused_while_power_cycling() {
  std::printf("an install can't start while a power cycle has the camera off\n");
  Bench b;
  b.power = true;
  b.line(WEDGED);
  CHECK(!b.power);
  b.m.start("1.0.0 " + HASH, b.now);
  CHECK(b.status() == "failed: camera power cycle running, try again in 10 s");
  CHECK(b.m.step() == InstallMachine::Step::IDLE);
  CHECK(b.power_offs == 1);
  b.advance(11000);
  CHECK(b.power);
  b.m.start("1.0.0 " + HASH, b.now);
  CHECK(b.status() == "starting camera");
  CHECK(b.power_offs == 2);
  CHECK(b.events.size() == 2 && b.events[1] == "camera power-cycled: install round 1");
}

static void test_millis_wrap() {
  std::printf("backoff and the manual gap survive the millis() wrap\n");
  Bench b;
  b.now = 0xFFFFFFFFu - 60 * 1000;  // one minute before the wrap
  b.line(WEDGED);  // #1 runs
  CHECK(b.power_offs == 1);
  b.advance(30 * 1000);
  CHECK(b.power);
  CHECK(b.m.power_cycle("button", b.now, true));  // first manual: allowed
  CHECK(b.power_offs == 2);
  b.advance(90 * 1000);  // across the wrap; 2 min gap not yet over
  CHECK(b.now < 60 * 1000);
  CHECK(!b.m.power_cycle("button", b.now, true));
  CHECK(b.power_offs == 2);
  b.line(WEDGED);  // auto #2: backed off 5 min
  CHECK(b.power_offs == 2);
  b.advance(60 * 1000);
  CHECK(b.m.power_cycle("button", b.now, true));  // 2.5 min after the first manual one
  CHECK(b.power_offs == 3);
  b.advance(5 * 60 * 1000);
  b.line(WEDGED);  // auto #2 after > 5 min
  CHECK(b.power_offs == 4);
  // An install whose rounds straddle the wrap keeps its timeouts.
  Bench c;
  c.now = 0xFFFFFFFFu - 5000;
  c.m.start("1.0.0 " + HASH, c.now);
  c.to_requested();  // crosses the wrap during POWER_OFF
  CHECK(c.sent_ota());
  CHECK(c.status() == "requesting firmware");
}

static void test_deferred_during_self_test() {
  std::printf("the self-test defers power cycles; a dropped request doesn't spend the backoff\n");
  Bench b;
  b.power = true;
  b.m.defer_power_cycles(true);
  CHECK(b.m.power_cycles_deferred());
  b.line(WEDGED);  // the watchdog asks mid-test: dropped
  CHECK(b.power && b.power_offs == 0 && b.events.empty());
  CHECK(!b.m.power_cycle("Camera Power Cycle button", b.now, true));  // the button too
  CHECK(b.power_offs == 0);
  b.advance(60 * 1000);
  b.m.defer_power_cycles(false);
  CHECK(!b.m.power_cycles_deferred());
  b.line(WEDGED);  // asked again after the test: runs at once (first cycle, no backoff spent)
  CHECK(!b.power && b.power_offs == 1);
  b.advance(InstallMachine::POWER_CYCLE_OFF_MS + 100);
  CHECK(b.power);
  // A cycle already under way when the test starts finishes on its own.
  Bench c;
  c.line(WEDGED);
  CHECK(!c.power);
  c.m.defer_power_cycles(true);
  c.advance(InstallMachine::POWER_CYCLE_OFF_MS + 100);
  CHECK(c.power);
}

int main() {
  test_timeout_during_power_off_restores_power();
  test_start_refused_while_power_cycling();
  test_millis_wrap();
  test_happy_path();
  test_flaky_wifi_retries_then_succeeds();
  test_dns_failure_after_a_200_is_not_a_refusal();
  test_server_refuses();
  test_gives_up_after_four_rounds();
  test_camera_silent();
  test_no_chamber_wifi();
  test_already_installed_and_other_automyco();
  test_automyco_camera_updates_without_power_cycle();
  test_automyco_camera_already_on_target();
  test_automyco_camera_refuses();
  test_automyco_camera_silent_falls_back_to_rounds();
  test_rejected_image_boots_stock();
  test_invalid_requests();
  test_overall_timeout();
  test_idle_keeps_automyco_camera_on_wifi();
  test_watchdog_power_cycle();
  test_watchdog_backoff();
  test_manual_cycle_and_install_guard();
  test_reason_is_sanitised();
  test_deferred_during_self_test();
  std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
