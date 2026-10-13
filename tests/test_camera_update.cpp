// Host tests for the AutoMyco camera's self-update rules (esphome/components/camera_update/update_logic.h).
//   cd esphome && g++ -std=c++17 -Wall -Wextra -Werror -I. -Icommon tests/test_camera_update.cpp -o /tmp/tcu && /tmp/tcu
#include "components/camera_update/update_logic.h"

#include <cstdio>
#include <algorithm>
#include <string>
#include <vector>

using namespace camera_update_logic;

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      failures++;                                                       \
    }                                                                   \
  } while (0)

static const std::string HASH = "33f51615f07a821b8d7f7ec8c972dfcaa85d682e2068df033732c2c5f73e8222";

static void test_versions() {
  CHECK(compare_versions("1.1.0", "1.0.0") > 0);
  CHECK(compare_versions("1.0.0", "1.1.0") < 0);
  CHECK(compare_versions("1.1", "1.1.0") == 0);
  CHECK(compare_versions("1.10.0", "1.9.0") > 0);           // numbers, not strings
  CHECK(compare_versions("1.1.0-rc1", "1.1.0") < 0);        // a pre-release is below its release
  CHECK(compare_versions("1.1.0-rc10", "1.1.0-rc9") > 0);   // same word: the trailing number decides
  CHECK(compare_versions("1.1.0-beta", "1.1.0-alpha") > 0); // different words: ASCII
  CHECK(compare_versions("1.1.0-rc.10", "1.1.0-rc.9") > 0); // numeric identifiers compare as numbers
  CHECK(compare_versions("1.1.0-1", "1.1.0-rc") < 0);       // numbers below words
  CHECK(compare_versions("1.1.0-rc.1", "1.1.0-rc") > 0);    // longer list wins
  CHECK(compare_versions("1.1.0+abc", "1.1.0") == 0);       // build metadata ignored
  CHECK(compare_versions("2.0.0", "1.99.99") > 0);
  CHECK(compare_versions("01.1.0", "1.1.0") == 0);
}

static void test_bare_version() {
  CHECK(bare_version("automyco-camera/1.1.0") == "1.1.0");
  CHECK(bare_version("1.1.0") == "1.1.0");
  CHECK(bare_version("camera/0.1.7.09") == "");             // a stock version is never ours
  CHECK(bare_version("automyco-camera/") == "");
  CHECK(bare_version("automyco-camera/1.1.0 ") == "");
  CHECK(bare_version("automyco-camera/../x") == "");
  CHECK(bare_version("1.1.0\"}") == "");
  CHECK(bare_version("v1.1.0") == "");                      // must start with a digit
  CHECK(bare_version(std::string(25, '1')) == "");
}

static void test_check() {
  CHECK(check("1.0.0", "1.1.0", HASH, false, false) == Verdict::OK);
  CHECK(check("1.0.1", "1.1.0", HASH, false, false) == Verdict::OK);
  CHECK(check("1.1.0", "1.1.0", HASH, false, false) == Verdict::SAME);
  CHECK(check("1.1.0", "1.0.0", HASH, false, false) == Verdict::DOWNGRADE);
  CHECK(check("1.1.0", "1.1.0-rc1", HASH, false, false) == Verdict::DOWNGRADE);
  CHECK(check("1.1.0", "1.0.0", HASH, true, false) == Verdict::OK);   // the owner chose it in the app
  CHECK(check("1.1.0", "1.1.0", HASH, true, false) == Verdict::OK);   // reinstall
  CHECK(check("1.0.0", "1.1.0", HASH, false, true) == Verdict::BUSY);
  CHECK(check("1.0.0", "", HASH, false, false) == Verdict::INVALID);
  CHECK(check("1.0.0", "1.1.0", "", false, false) == Verdict::INVALID);
  CHECK(check("1.0.0", "1.1.0", HASH.substr(1), false, false) == Verdict::INVALID);
  std::string upper = HASH;
  upper[0] = 'A';
  CHECK(check("1.0.0", "1.1.0", upper, false, false) == Verdict::INVALID);  // lowercase hex only
  CHECK(check("1.0.0", "1.1.0", HASH, true, true) == Verdict::BUSY);        // force doesn't skip busy
  CHECK(check("1.0.0", "x", HASH, true, false) == Verdict::INVALID);        // nor validation
  CHECK(verdict_reason(Verdict::SAME, "1.1.0") == "already on 1.1.0");
  CHECK(verdict_reason(Verdict::DOWNGRADE, "1.1.0") == "older than 1.1.0");
}

static void test_url_and_request_size() {
  CHECK(download_url("api.automyco.com", HASH) == "https://api.automyco.com/fw/" + HASH + ".bin");
  // The 512 B default TX buffer broke the main board's update; ours is TX_BUFFER and the request
  // must stay far inside it.
  const size_t bytes = request_bytes("api.automyco.com", HASH);
  CHECK(bytes < 200);
  CHECK(bytes * 2 < TX_BUFFER);
  CHECK(request_bytes(std::string(64, 'a'), HASH) < TX_BUFFER);
  CHECK(valid_host("api.automyco.com"));
  CHECK(!valid_host("api.automyco.com/evil"));
  CHECK(!valid_host("https://api.automyco.com"));
  CHECK(!valid_host("api.automyco.com:444"));
  CHECK(!valid_host(""));
  CHECK(!valid_host(".com"));
  CHECK(!valid_host("a..b"));
}

static void test_heap_gate() {
  // The camera bench numbers aren't in yet; these are the main board's (common/tls_budget.h) plus our
  // read buffer and task stack.
  CHECK(heap_ok(32 * 1024, 120 * 1024));
  CHECK(!heap_ok(16 * 1024, 120 * 1024));  // the 16.7 KB IN record buffer needs one block
  CHECK(!heap_ok(32 * 1024, 50 * 1024));   // 48 KB TLS + 4 KB buffer + 8 KB stack
  CHECK(heap_ok(TLS_BLOCK_NEEDED, TLS_FREE_NEEDED + READ_BUFFER + TASK_STACK));
}

static void test_progress_and_lines() {
  CHECK(percent(0, 0) == 0);
  CHECK(percent(50, 100) == 50);
  CHECK(percent(1200000, 1200000) == 100);
  CHECK(percent(2, 1) == 100);
  CHECK(percent(4000000000u, 4000000001u) == 99);  // no 32-bit overflow
  CHECK(progress_step(-1, 0));
  CHECK(!progress_step(0, 9));
  CHECK(progress_step(9, 10));
  CHECK(!progress_step(10, 19));
  CHECK(progress_step(95, 100));
  CHECK(link_line(State::DOWNLOADING, 40, "1.1.0", "") ==
        "{\"status\":\"ota\",\"state\":\"downloading\",\"progress\":40,\"version\":\"1.1.0\"}");
  CHECK(link_line(State::FAILED, 0, "1.1.0", "image hash did not match") ==
        "{\"status\":\"ota\",\"state\":\"failed\",\"version\":\"1.1.0\",\"error\":\"image hash did not match\"}");
  CHECK(link_line(State::DONE, 100, "1.1.0", "") == "{\"status\":\"ota\",\"state\":\"installed\",\"version\":\"1.1.0\"}");
  // Nothing from a request can break out of the JSON string.
  CHECK(link_line(State::FAILED, 0, "", "a\"b\\c\nd") == "{\"status\":\"ota\",\"state\":\"failed\",\"error\":\"abcd\"}");
  CHECK(status_json(State::IDLE, 0, "1.1.0", "", "stale") ==
        "{\"state\":\"idle\",\"progress\":0,\"running\":\"1.1.0\",\"version\":\"\",\"error\":\"\"}");
  CHECK(status_json(State::FAILED, 0, "1.0.1", "1.1.0", "release not found on the server") ==
        "{\"state\":\"failed\",\"progress\":0,\"running\":\"1.0.1\",\"version\":\"1.1.0\",\"error\":\"release not found on the server\"}");
  CHECK(safe_text(std::string(100, 'x')).size() == 60);
}

static void test_retry_policy() {
  CHECK(Attempt::retryable(0, true));      // no connection, DNS, TLS, cut off
  CHECK(Attempt::retryable(503, false));
  CHECK(Attempt::retryable(429, false));
  CHECK(!Attempt::retryable(404, false));  // not published: asking again won't change it
  CHECK(!Attempt::retryable(403, false));
  CHECK(!Attempt::retryable(200, false));  // got the file, it was wrong
}

// An app image as the camera downloads it: image header (0xE9), segment header, app descriptor, then
// somewhere later ESPHome's project string.
static std::vector<uint8_t> fake_image(const std::string &desc_version, const std::string &project_version,
                                       const std::string &project = "automyco-camera") {
  std::vector<uint8_t> img(4096, 0xAA);
  img[0] = 0xE9;
  uint8_t *d = img.data() + APP_DESC_OFFSET;
  d[0] = 0x32; d[1] = 0x54; d[2] = 0xCD; d[3] = 0xAB;  // ABCD5432, little-endian
  std::fill(d + 16, d + 80, 0);
  std::copy(desc_version.begin(), desc_version.end(), d + 16);
  std::copy(project.begin(), project.end(), d + 48);
  const std::string tag = std::string("Project community.automyco-camera version ") + project_version;
  std::copy(tag.begin(), tag.end(), img.begin() + 3000);
  img[3000 + tag.size()] = 0;
  return img;
}

struct Downloaded {
  AppDesc desc;
  std::string project_version;
};

// Feeds the image in `chunk`-byte reads, like the download loop.
static Downloaded download(const std::vector<uint8_t> &img, size_t chunk) {
  ProjectVersionScanner scanner;
  uint8_t head[APP_DESC_MIN_BYTES];
  size_t head_len = 0;
  for (size_t at = 0; at < img.size(); at += chunk) {
    const size_t n = std::min(chunk, img.size() - at);
    if (head_len < sizeof(head)) {
      const size_t take = std::min(sizeof(head) - head_len, n);
      std::copy(img.begin() + at, img.begin() + at + take, head + head_len);
      head_len += take;
    }
    scanner.feed(img.data() + at, n);
  }
  return {parse_app_desc(head, head_len), scanner.version()};
}

static std::string verdict(const std::vector<uint8_t> &img, const std::string &running, const std::string &requested,
                           bool force = false) {
  const Downloaded got = download(img, 4096);
  return image_verdict(got.desc, got.project_version, "automyco-camera", running, requested, force);
}

static void test_image_version_decides() {
  // The review's attack: a high fake version in the request, an old published release's hash.
  CHECK(verdict(fake_image("1.0.1", "1.0.1"), "1.1.0", "9.9.9") != "");
  CHECK(verdict(fake_image("1.0.1", "1.0.1"), "1.1.0", "1.0.1") == "older than 1.1.0");
  // Equal to what runs: refused.
  CHECK(verdict(fake_image("1.1.0", "1.1.0"), "1.1.0", "1.1.0") == "already on 1.1.0");
  // Newer, and what was asked for: accepted.
  CHECK(verdict(fake_image("1.2.0", "1.2.0"), "1.1.0", "1.2.0") == "");
  // Newer, but not what the request said: refused (the request must name the image it gets).
  CHECK(verdict(fake_image("1.2.0", "1.2.0"), "1.1.0", "1.3.0") == "image is 1.2.0, not 1.3.0");
  // An image from before the descriptor carried camera_version reads as the ESPHome version, which
  // would compare as newer than anything; the project string disagrees, so it's refused.
  CHECK(verdict(fake_image("2026.9.1", "1.0.0"), "1.1.0", "2026.9.1") == "image has no camera version");
  CHECK(verdict(fake_image("2026.9.1", "1.0.0"), "1.1.0", "2026.9.1", true) == "image has no camera version");
  // No project string at all.
  std::vector<uint8_t> bare = fake_image("1.2.0", "1.2.0");
  std::fill(bare.begin() + 3000, bare.begin() + 3060, 0xAA);
  CHECK(verdict(bare, "1.1.0", "1.2.0") == "image has no camera version");
  // Another product, a broken header or descriptor.
  CHECK(verdict(fake_image("1.2.0", "1.2.0", "terrashroom"), "1.1.0", "1.2.0") == "not an AutoMyco camera image");
  std::vector<uint8_t> no_magic = fake_image("1.2.0", "1.2.0");
  no_magic[APP_DESC_OFFSET] = 0;
  CHECK(verdict(no_magic, "1.1.0", "1.2.0") == "not an AutoMyco camera image");
  std::vector<uint8_t> not_app = fake_image("1.2.0", "1.2.0");
  not_app[0] = 0;
  CHECK(verdict(not_app, "1.1.0", "1.2.0") == "not an AutoMyco camera image");
  // force (camera login only) allows a reinstall or downgrade of a real camera image, nothing else.
  CHECK(verdict(fake_image("1.0.1", "1.0.1"), "1.1.0", "1.0.1", true) == "");
  CHECK(verdict(fake_image("1.1.0", "1.1.0"), "1.1.0", "1.1.0", true) == "");
}

static void test_project_string_across_chunks() {
  const std::vector<uint8_t> img = fake_image("1.2.0", "1.2.0");
  for (size_t chunk : {1u, 7u, 40u, 3001u, 3020u, 3044u, 4096u}) {
    const Downloaded got = download(img, chunk);
    CHECK(got.desc.ok && got.desc.version == "1.2.0" && got.desc.project == "automyco-camera");
    CHECK(got.project_version == "1.2.0");
  }
  // Too long to be a version: ignored, and a real one after it is still found.
  std::vector<uint8_t> two = fake_image("1.2.0", std::string(40, '9'));
  const std::string tag = "Project community.automyco-camera version 1.2.0";
  std::copy(tag.begin(), tag.end(), two.begin() + 3500);
  two[3500 + tag.size()] = 0;
  CHECK(download(two, 64).project_version == "1.2.0");
  CHECK(!parse_app_desc(img.data(), APP_DESC_MIN_BYTES - 1).ok);
}

int main() {
  test_image_version_decides();
  test_project_string_across_chunks();
  test_versions();
  test_bare_version();
  test_check();
  test_url_and_request_size();
  test_heap_gate();
  test_progress_and_lines();
  test_retry_policy();
  if (failures) {
    std::printf("test_camera_update: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("test_camera_update: all passed\n");
  return 0;
}
