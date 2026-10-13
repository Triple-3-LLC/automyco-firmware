// Host tests for esphome/components/camera_http/claim_guard.h (who may POST :8080/claim).
//   cd esphome && g++ -std=c++17 -Wall -Wextra -Werror -I. -Icommon tests/test_claim_guard.cpp -o /tmp/t && /tmp/t
#include "components/camera_http/claim_guard.h"

#include <cstdio>

using namespace esphome::camera_http::claim_guard;

static int failures = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                   \
    }                                                               \
  } while (0)

static const std::string kName = "automyco-camera-a1b2c3";

static void test_app_request_passes() {
  // What apple/AutoMyco CameraPairing sends: http://<camera IP>:8080/claim, JSON, no Origin.
  CHECK(refusal("192.168.1.21:8080", false, "application/json", kName) == nullptr);
  CHECK(refusal("192.168.1.21", false, "application/json; charset=utf-8", kName) == nullptr);
  CHECK(refusal("automyco-camera-a1b2c3.local:8080", false, "Application/JSON", kName) == nullptr);
  CHECK(refusal("AUTOMYCO-CAMERA-A1B2C3.LOCAL.", false, "application/json", kName) == nullptr);
  CHECK(refusal("[fe80::1]:8080", false, "application/json", kName) == nullptr);
}

static void test_web_pages_refused() {
  CHECK(refusal("192.168.1.21:8080", true, "application/json", kName) != nullptr);        // any Origin
  CHECK(refusal("192.168.1.21:8080", false, "text/plain", kName) != nullptr);             // simple request
  CHECK(refusal("192.168.1.21:8080", false, "application/x-www-form-urlencoded", kName) != nullptr);
  CHECK(refusal("192.168.1.21:8080", false, "multipart/form-data; boundary=x", kName) != nullptr);
  CHECK(refusal("192.168.1.21:8080", false, "", kName) != nullptr);
  CHECK(refusal("192.168.1.21:8080", false, "application/jsonp", kName) != nullptr);
  CHECK(refusal("evil.example:8080", false, "application/json", kName) != nullptr);       // DNS rebinding
  CHECK(refusal("automyco-camera-a1b2c3.evil.example", false, "application/json", kName) != nullptr);
  CHECK(refusal("automyco-camera-ffffff.local", false, "application/json", kName) != nullptr);   // not me
  CHECK(refusal("", false, "application/json", kName) != nullptr);
  CHECK(refusal("192.168.1.256", false, "application/json", kName) != nullptr);
  CHECK(refusal("192.168.1.21:80x", false, "application/json", kName) != nullptr);
  CHECK(refusal("[fe80::1", false, "application/json", kName) != nullptr);
  CHECK(refusal("192.168.1.21:", false, "application/json", kName) != nullptr);
}

int main() {
  test_app_request_passes();
  test_web_pages_refused();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("claim_guard: all checks passed\n");
  return 0;
}
