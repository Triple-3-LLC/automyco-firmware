// Host tests for esphome/components/web_guard/control_key.h: the X-AutoMyco-Key header a paired phone sends
// (HMAC over a single-use challenge) and which requests need it.
//   cd esphome && g++ -std=c++17 -Wall -Wextra -I. -Icommon tests/test_control_key.cpp -o /tmp/tst && /tmp/tst
// SHA-256: FIPS 180-2 plus padding edges; HMAC: RFC 4231 cases 2 and 6. The header vector is shared with
// apple/AutoMyco/ShroomChamberTests/ControlKeyTests.swift and apple/AutoMyco/mock_server/test_esphome_mock.py (all three agree).
#include "components/web_guard/control_key.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace esphome::web_guard;

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      failures++;                                                       \
    }                                                                   \
  } while (0)


// Test fixture, not a real credential.
static const std::string kPairing = "Abc123def456GHI789jkl012MNO345pq";  // gitleaks:allow
static const std::string kChallenge = "00112233445566778899aabbccddeeff";
static const std::string kNext = "ffeeddccbbaa99887766554433221100";
static const std::string kTarget = "/switch/UV-C%20Lamp/turn_on";

static KeyGate paired() {
  KeyGate g;
  gate_set_password(g, kPairing, kChallenge);
  return g;
}

static void test_hashes() {
  CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  CHECK(sha256_hex(std::string(1000, 'a')) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
  CHECK(sha256_hex(std::string(55, 'x')) == "d5e285683cd4efc02d021a5c62014694958901005d6f71e89e0989fac77e4072");
  CHECK(sha256_hex(std::string(56, 'x')) == "04c26261370ee7541549d16dee320c723e3fd14671e66a099afe0a377c16888e");
  CHECK(sha256_hex(std::string(63, 'x')) == "75220b47218278e656f2013bb8f0c455a25eaf01e86c64924e9d48d89776d6f2");
  CHECK(sha256_hex(std::string(64, 'x')) == "7ce100971f64e7001e8fe5a51973ecdfe1ced42befe7ee8d5fd6219506b5393c");
  CHECK(sha256_hex(std::string(65, 'x')) == "9537c5fdf120482f7d58d25e9ed583f52c02b4e304ea814db1633ad565aed7e9");
  CHECK(to_hex(hmac_sha256_raw("Jefe", "what do ya want for nothing?")) ==
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  CHECK(to_hex(hmac_sha256_raw(std::string(131, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First")) ==
        "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

static void test_header() {
  CHECK(to_hex(control_key(kPairing)) == "5ca69929b735bc734777e436e2ba9068d566f0cfa648cb0f85e808cc619e2830");  // gitleaks:allow
  CHECK(control_key("").empty());
  const std::string h = control_header(control_key(kPairing), "POST", kTarget, kChallenge);
  CHECK(h == "ed969c9741eb7770d127b987df978c5ec5de9a7a3bceb164c1054c8a9e9397b1");  // gitleaks:allow

  // The right challenge is accepted once; the challenge then rotates.
  KeyGate g = paired();
  CHECK(gate_check(g, "POST", kTarget, h, kNext) == KeyCheck::OK);
  CHECK(g.challenge == kNext);
  // Reuse after rotation: the same header is dead.
  CHECK(gate_check(g, "POST", kTarget, h, kChallenge) == KeyCheck::BAD_MAC);
  CHECK(g.challenge == kNext);   // a refusal doesn't rotate
  // A fresh header over the new challenge works.
  CHECK(gate_check(g, "POST", kTarget, control_header(control_key(kPairing), "POST", kTarget, kNext), kChallenge) ==
        KeyCheck::OK);

  // Wrong path or method, wrong key, stale challenge, malformed.
  g = paired();
  CHECK(gate_check(g, "POST", "/switch/UV-C%20Lamp/toggle", h, kNext) == KeyCheck::BAD_MAC);
  CHECK(gate_check(g, "POST", "/switch/UV-C Lamp/turn_on", h, kNext) == KeyCheck::BAD_MAC);   // decoded != as sent
  CHECK(gate_check(g, "POST", "/button/Camera%20Reset%20Login/press", h, kNext) == KeyCheck::BAD_MAC);
  CHECK(gate_check(g, "PUT", kTarget, h, kNext) == KeyCheck::BAD_MAC);
  CHECK(gate_check(g, "POST", kTarget, control_header(control_key("someone-else-entirely"), "POST", kTarget, kChallenge),
                   kNext) == KeyCheck::BAD_MAC);
  CHECK(gate_check(g, "POST", kTarget, control_header(control_key(kPairing), "POST", kTarget, kNext), kNext) ==
        KeyCheck::BAD_MAC);   // signed over a challenge that isn't current
  CHECK(gate_check(g, "POST", kTarget, "", kNext) == KeyCheck::BAD_MAC);
  CHECK(gate_check(g, "POST", kTarget, h.substr(0, 63), kNext) == KeyCheck::BAD_MAC);
  CHECK(gate_check(g, "POST", kTarget, h + "0", kNext) == KeyCheck::BAD_MAC);
  CHECK(g.challenge == kChallenge);   // nothing refused rotated it
  std::string upper = h;
  for (auto &c : upper)
    if (c >= 'a' && c <= 'f')
      c = (char) (c - 32);
  CHECK(gate_check(g, "POST", kTarget, upper, kNext) == KeyCheck::OK);   // hex case doesn't matter

  // Constant-time compare: same answer wherever the difference is, and length/empty edges.
  const std::string e(64, 'a');
  std::string first = e, last = e;
  first[0] = 'b';
  last[63] = 'b';
  CHECK(!ct_equal_hex(e, first));
  CHECK(!ct_equal_hex(e, last));
  CHECK(ct_equal_hex(e, std::string(64, 'A')));
  CHECK(!ct_equal_hex(std::string(64, 'g'), std::string(64, 'G')));
  CHECK(!ct_equal_hex(e, std::string(64, '!')));
  CHECK(!ct_equal_hex("", ""));
  CHECK(!ct_equal_hex(e, e.substr(1)));
}

static void test_gate() {
  // Boot: until the password has loaded (with the radio up), keyed actions are refused, not open.
  KeyGate g;
  CHECK(gate_check(g, "POST", kTarget, "", kNext) == KeyCheck::NOT_LOADED);
  gate_set_password(g, kPairing, "");   // no real challenge yet: still not loaded
  CHECK(gate_check(g, "POST", kTarget, "", kNext) == KeyCheck::NOT_LOADED);

  // Loaded and empty: nobody has paired, open, and the challenge isn't consumed.
  gate_set_password(g, "", kChallenge);
  CHECK(gate_check(g, "POST", kTarget, "", kNext) == KeyCheck::OK);
  CHECK(g.challenge == kChallenge);

  // Replay across a reboot: the new boot has a fresh random challenge, so a header sniffed before it is dead.
  const std::string sniffed = control_header(control_key(kPairing), "POST", kTarget, kChallenge);
  KeyGate rebooted;
  gate_set_password(rebooted, kPairing, kNext);
  CHECK(gate_check(rebooted, "POST", kTarget, sniffed, kChallenge) == KeyCheck::BAD_MAC);

  // Re-pair (same password) or set_ota_password: a new challenge, the old header is dead too.
  KeyGate repaired = paired();
  gate_set_password(repaired, kPairing, kNext);
  CHECK(gate_check(repaired, "POST", kTarget, sniffed, kChallenge) == KeyCheck::BAD_MAC);
  CHECK(gate_check(repaired, "POST", kTarget, control_header(control_key(kPairing), "POST", kTarget, kNext),
                   kChallenge) == KeyCheck::OK);
}

static void test_keyed_requests() {
  const std::vector<KeyedEntity> keyed = {{"uv-c lamp", "uv-c_lamp", "turn_off"},
                                          {"camera reset login", "camera_reset_login", ""}};
  // Paths are as web_server sees them: decoded once by url_to (pct_decode here), query already stripped.
  auto gated = [&](const std::string &raw) { return keyed_request(pct_decode(raw), keyed); };
  // Every spelling of UV-C Lamp switching on.
  for (const char *p : {"/switch/UV-C Lamp/turn_on", "/switch/UV-C%20Lamp/turn_on", "/switch/UV-C+Lamp/turn_on",
                        "/switch/uv-c lamp/turn_on", "/switch/UV-C%20LAMP/turn_on", "/switch/UV-C%2520Lamp/turn_on",
                        "/switch/uv-c_lamp/turn_on", "/switch/switch-uv-c_lamp/turn_on", "/switch/UV-C%20Lamp/toggle",
                        "/switch/UV-C%20Lamp/turn_on/", "/switch/UV-C%20Lamp//turn_on", "/switch/../switch/UV-C%20Lamp/turn_on",
                        "/switch/UV-C%20Lamp/turn_off/../turn_on", "/switch/main/UV-C%20Lamp/turn_on",
                        "/light/UV-C%20Lamp/turn_on", "/switch/UV-C%20Lamp/set", "/switch/UV-C%20Lamp", "/switch/UV-C%20Lamp/"})
    CHECK(gated(p) || (std::printf("    not gated: %s\n", p), false));
  // Turning it off stays open.
  CHECK(!gated("/switch/UV-C%20Lamp/turn_off"));
  CHECK(!gated("/switch/uv-c_lamp/turn_off"));
  // Camera Reset Login: every action, every spelling.
  for (const char *p : {"/button/Camera%20Reset%20Login/press", "/button/Camera+Reset+Login/press",
                        "/button/camera_reset_login/press", "/button/button-camera_reset_login/press",
                        "/button/CAMERA%20RESET%20LOGIN/press/", "/button/Camera%2520Reset%2520Login/press",
                        "/button/Camera%20Reset%20Login/turn_off"})
    CHECK(gated(p) || (std::printf("    not gated: %s\n", p), false));
  // Unrelated entities never are.
  for (const char *p : {"/switch/Heater/turn_on", "/switch/Climate%20Control/turn_on", "/light/Top%20Light/toggle",
                        "/number/UV-C%20Auto-Off/set", "/sensor/UV-C%20Remaining", "/button/Camera%20Power%20Cycle/press",
                        "/button/Start%20Grow/press", "/events", "/"})
    CHECK(!gated(p) || (std::printf("    gated by mistake: %s\n", p), false));
}

// A held-back header dies when the challenge expires; expiry leaves unpaired and unloaded gates alone.
static void test_expiry() {
  KeyGate g = paired();
  const std::string h = control_header(control_key(kPairing), "POST", kTarget, kChallenge);
  CHECK(gate_expire(g, kNext));
  CHECK(g.challenge == kNext);
  CHECK(gate_check(g, "POST", kTarget, h, kChallenge) == KeyCheck::BAD_MAC);
  CHECK(!gate_expire(g, "short"));   // a bad replacement is ignored
  CHECK(g.challenge == kNext);

  KeyGate unpaired;
  gate_set_password(unpaired, "", kChallenge);
  CHECK(!gate_expire(unpaired, kNext));
  CHECK(unpaired.challenge == kChallenge);
  KeyGate unloaded;
  CHECK(!gate_expire(unloaded, kNext));
  CHECK(unloaded.challenge.empty());
}

int main() {
  test_hashes();
  test_expiry();
  test_header();
  test_gate();
  test_keyed_requests();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("control_key: all checks passed\n");
  return 0;
}
