// Host tests for esphome/components/flash_dumper (the donor camera stock-firmware dumper).
//   cd esphome && g++ -std=c++17 -Wall -Wextra -Werror -I. -Icommon tests/test_flash_dumper.cpp -o /tmp/tfd && /tmp/tfd
// Covers the pure decisions in dump_logic.h (which slot to dump, how long the image is) and the
// component's read-only guarantee (no flash write/erase/OTA/boot-switch call in its sources).
// Run from esphome/ (CI does), so the source scan finds components/flash_dumper/.
#include "components/flash_dumper/dump_logic.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace dump_logic;

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      failures++;                                                       \
    }                                                                   \
  } while (0)

// Arduino's default camera-style table (two 1.25 MB slots) and the stock main board's (1728K slots).
static const std::vector<AppSlot> ARDUINO = {{"app0", 0x10000, 0x140000}, {"app1", 0x150000, 0x140000}};
static const std::vector<AppSlot> MAIN = {{"app0", 0x10000, 0x1B0000}, {"app1", 0x1C0000, 0x1B0000}};

static void test_slot_selection() {
  // The case that matters: the stock updater put the dumper in app1, so the stock image is in app0.
  Choice c = pick_dump_slot(ARDUINO, 0x150000);
  CHECK(c.result == Pick::OK);
  CHECK(c.running == 1);
  CHECK(c.index == 0);
  CHECK(ARDUINO[c.index].label == "app0");

  // A camera Terrashroom once updated runs stock from app1; the dumper then lands in app0.
  c = pick_dump_slot(ARDUINO, 0x10000);
  CHECK(c.result == Pick::OK && c.index == 1);
  c = pick_dump_slot(MAIN, 0x1C0000);
  CHECK(c.result == Pick::OK && c.index == 0);

  // Never the running slot, whatever the order of the table.
  const std::vector<AppSlot> reversed = {ARDUINO[1], ARDUINO[0]};
  c = pick_dump_slot(reversed, 0x150000);
  CHECK(c.result == Pick::OK && reversed[c.index].address == 0x10000);

  // Refusals.
  CHECK(pick_dump_slot({}, 0x10000).result == Pick::NO_APPS);
  CHECK(pick_dump_slot(ARDUINO, 0x200000).result == Pick::RUNNING_UNKNOWN);
  CHECK(pick_dump_slot({{"factory", 0x10000, 0x300000}}, 0x10000).result == Pick::NO_OTHER_SLOT);

  // factory + ota_0 + ota_1: running ota_1 -> its counterpart ota_0, never factory.
  const std::vector<AppSlot> three = {{"factory", 0x10000, 0x100000}, {"ota_0", 0x110000, 0x100000},
                                      {"ota_1", 0x210000, 0x100000}};
  c = pick_dump_slot(three, 0x210000);
  CHECK(c.result == Pick::OK && three[c.index].label == "ota_0");
  // Running factory has no counterpart: refuse rather than guess between ota_0 and ota_1.
  CHECK(pick_dump_slot(three, 0x10000).result == Pick::AMBIGUOUS);
  // Duplicate counterpart labels: refuse.
  const std::vector<AppSlot> dup = {{"app0", 0x10000, 0x100}, {"app1", 0x20000, 0x100}, {"app1", 0x30000, 0x100}};
  CHECK(pick_dump_slot(dup, 0x10000).result == Pick::AMBIGUOUS);

  CHECK(counterpart("app0") == "app1" && counterpart("ota_1") == "ota_0" && counterpart("nvs").empty());
}

// ---- Synthetic ESP32 app images (the layout esptool elf2image writes) ----

static void put32(std::vector<uint8_t> &v, size_t at, uint32_t x) {
  for (int i = 0; i < 4; i++)
    v[at + i] = uint8_t(x >> (8 * i));
}

// Header + segments + padding + checksum (+ 32-byte digest placeholder). Returns the image; the
// caller pads it out to a partition with 0xFF, as flash looks after an OTA.
static std::vector<uint8_t> make_image(const std::vector<uint32_t> &seg_lens, bool hash_appended) {
  std::vector<uint8_t> img(HEADER_LEN, 0);
  img[0] = IMAGE_MAGIC;
  img[1] = uint8_t(seg_lens.size());
  img[2] = 2;     // DIO
  img[3] = 0x20;  // 40 MHz, 4 MB
  put32(img, 4, 0x40081234);
  img[23] = hash_appended ? 1 : 0;
  uint32_t load = 0x3F400020;
  for (uint32_t len : seg_lens) {
    const size_t at = img.size();
    img.resize(at + SEGMENT_HEADER_LEN + len, 0x5A);
    put32(img, at, load);
    put32(img, at + 4, len);
    load += 0x10000;
  }
  const size_t end = checksum_end(uint32_t(img.size()));
  img.resize(end, 0);
  img[end - 1] = 0xEF;  // checksum value doesn't matter to the length logic
  if (hash_appended)
    img.resize(end + DIGEST_LEN, 0xAB);
  return img;
}

static ImageInfo parse(const std::vector<uint8_t> &flash, uint32_t part_size, int *reads_past = nullptr) {
  return parse_image(
      [&](uint32_t off, uint8_t *buf, size_t len) {
        if (off + len > part_size || off + len > flash.size()) {
          if (reads_past)
            (*reads_past)++;
          return false;
        }
        std::copy(flash.begin() + off, flash.begin() + off + len, buf);
        return true;
      },
      part_size);
}

static void test_image_length() {
  // checksum_end: the checksum byte is the last byte of a 16-byte block.
  CHECK(checksum_end(31) == 32);
  CHECK(checksum_end(32) == 48);
  CHECK(checksum_end(33) == 48);
  CHECK(checksum_end(47) == 48);

  const uint32_t part = 0x140000;
  for (bool hashed : {true, false}) {
    for (std::vector<uint32_t> segs : {std::vector<uint32_t>{4}, {0x1000, 0x20, 0x7FFC}, {16, 32, 64, 128, 256, 4}}) {
      std::vector<uint8_t> img = make_image(segs, hashed);
      const size_t real = img.size();
      img.resize(part, 0xFF);  // rest of the slot: erased flash
      ImageInfo info = parse(img, part);
      CHECK(info.ok);
      CHECK(info.length == real);
      CHECK(info.length % 16 == 0);
      CHECK(info.hash_appended == hashed);
      CHECK(info.segments == segs.size());
      CHECK(info.entry == 0x40081234);
      if (hashed)
        CHECK(info.digest_offset == real - DIGEST_LEN);
      CHECK(serve_length(info, part, true) == real);
      CHECK(serve_length(info, part, false) == part);
    }
  }

  // A realistic stock-sized image: 1,260,672 B like the main board's stock app0 (LOG.md).
  {
    std::vector<uint8_t> img = make_image({0x30000, 0x80000, 0x2000, 0x8BF4C}, true);
    const size_t real = img.size();
    img.resize(part, 0xFF);
    ImageInfo info = parse(img, part);
    CHECK(info.ok && info.length == real && real < part);
  }

  // Refusals: erased slot, garbage, oversized segment, image longer than the slot, odd lengths.
  int past = 0;
  std::vector<uint8_t> erased(part, 0xFF);
  ImageInfo info = parse(erased, part, &past);
  CHECK(!info.ok && serve_length(info, part, true) == 0);
  CHECK(serve_length(info, part, false) == part);  // the raw slot is still available on request

  std::vector<uint8_t> img = make_image({0x100}, true);
  img[1] = 0;
  img.resize(part, 0xFF);
  CHECK(!parse(img, part).ok);
  img[1] = 17;
  CHECK(!parse(img, part).ok);

  img = make_image({0x100}, true);
  img[23] = 7;
  img.resize(part, 0xFF);
  CHECK(!parse(img, part).ok);

  img = make_image({0x100}, false);
  put32(img, HEADER_LEN + 4, 0x7FFFFFF0);  // segment claims ~2 GB
  img.resize(part, 0xFF);
  past = 0;
  CHECK(!parse(img, part, &past).ok);
  CHECK(past == 0);  // rejected from the header, without reading past the slot

  img = make_image({0x100}, false);
  put32(img, HEADER_LEN + 4, 0x102);  // not a multiple of 4
  img.resize(part, 0xFF);
  CHECK(!parse(img, part).ok);

  // Image fits only without its digest: still refused (the digest is part of the file).
  img = make_image({0x1000}, true);
  const uint32_t small_part = uint32_t(img.size()) - 16;
  CHECK(!parse(img, small_part).ok);
  CHECK(parse(img, uint32_t(img.size())).ok);

  // Tiny "partition".
  CHECK(!parse(img, 8).ok);
}

static void test_helpers() {
  const uint8_t d[4] = {0x12, 0x7f, 0xa0, 0x0e};
  CHECK(to_hex(d, 4) == "127fa00e");
  CHECK(header_matches("Basic ZHVtcGVyOnNlY3JldA==", "Basic ZHVtcGVyOnNlY3JldA=="));
  CHECK(!header_matches("Basic ZHVtcGVyOnNlY3JldA", "Basic ZHVtcGVyOnNlY3JldA=="));
  CHECK(!header_matches("Basic ZHVtcGVyOnNlY3JldA==x", "Basic ZHVtcGVyOnNlY3JldA=="));
  CHECK(!header_matches("", "Basic ZHVtcGVyOnNlY3JldA=="));
  const char field[8] = {'v', '1', '"', '\\', '\n', 'x', 'y', 'z'};  // no NUL, and JSON-hostile bytes
  CHECK(desc_field(field, sizeof(field)) == "v1???xyz");
}

// The read-only guarantee, checked on the sources themselves: none of the ESP-IDF calls that write,
// erase or switch the boot slot may appear outside the poison list in flash_dumper.cpp.
static std::string slurp(const char *path) {
  std::ifstream f(path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static void test_never_writes_flash() {
  static const char *const FORBIDDEN[] = {
      "esp_partition_write", "esp_partition_erase_range", "esp_ota_begin", "esp_ota_write", "esp_ota_end",
      "esp_ota_abort", "esp_ota_set_boot_partition", "esp_ota_erase_last_boot_app_partition",
      "esp_ota_mark_app_valid_cancel_rollback", "esp_ota_mark_app_invalid_rollback_and_reboot", "esp_flash_write",
      "esp_flash_erase_region", "esp_flash_erase_chip", "spi_flash_write", "spi_flash_erase_range",
      "spi_flash_erase_sector", "nvs_set", "nvs_commit", "nvs_erase"};
  static const char *const FILES[] = {"components/flash_dumper/flash_dumper.cpp",
                                      "components/flash_dumper/flash_dumper.h",
                                      "components/flash_dumper/dump_logic.h"};
  for (const char *path : FILES) {
    const std::string src = slurp(path);
    CHECK(!src.empty());
    if (src.empty()) {
      std::printf("  (could not read %s; run from esphome/)\n", path);
      continue;
    }
    std::istringstream lines(src);
    std::string line;
    int n = 0;
    while (std::getline(lines, line)) {
      n++;
      if (line.find("#pragma GCC poison") != std::string::npos)
        continue;
      for (const char *bad : FORBIDDEN) {
        if (line.find(bad) != std::string::npos) {
          std::printf("  FAIL %s:%d uses %s\n", path, n, bad);
          failures++;
        }
      }
    }
  }
  // And the poison list itself is present, so the compiler enforces it too.
  const std::string cpp = slurp("components/flash_dumper/flash_dumper.cpp");
  for (const char *must : {"esp_partition_write", "esp_partition_erase_range", "esp_ota_begin", "esp_ota_set_boot_partition",
                           "esp_flash_write", "spi_flash_write"}) {
    bool poisoned = false;
    std::istringstream lines(cpp);
    std::string line;
    while (std::getline(lines, line))
      if (line.find("#pragma GCC poison") != std::string::npos && line.find(must) != std::string::npos)
        poisoned = true;
    CHECK(poisoned);
  }
}


// ---- Authentication on every route (security review, PR #147) ----
// A model of the on-device handler: it runs plan_request() first and touches "flash" (and sends its
// bytes) only on 200, exactly like info_handler_/app_handler_/unlock_handler_ via authorized_().
static const char *const GOOD_HDR = "Basic ZHVtcGVyOmQzN2JhNzY0M2JjZDQwY2QxZjNmNzA2OTgyMGI1ODgx";  // dumper:<32 hex>
static const std::string GOOD_PW = "d37ba7643bcd40cd1f3f7069820b5881";

struct Served {
  int status;
  int flash_reads;
  size_t flash_bytes_sent;
};

static Served serve(Method m, const std::string &uri, HeaderState st, const char *hdr, bool configured,
                    const char *expected) {
  Served out{0, 0, 0};
  const Plan plan = plan_request(m, uri, st, hdr, configured, expected);
  out.status = plan.status;
  if (plan.status != 200)
    return out;  // 401/404: fixed text body, nothing from flash
  out.flash_reads = 1;
  out.flash_bytes_sent = plan.route == Route::APP ? 4096 : 0;
  return out;
}

static void test_auth_every_route() {
  struct R { Method m; const char *uri; Route route; };
  const R routes[] = {{Method::GET, "/dump/info", Route::INFO},
                      {Method::GET, "/dump/app", Route::APP},
                      {Method::GET, "/dump/app?trim=0", Route::APP},
                      {Method::POST, "/dump/ota-unlock", Route::UNLOCK},
                      {Method::POST, "/dump/ota-unlock?force=1", Route::UNLOCK}};
  const bool cfg = login_configured(GOOD_PW, GOOD_HDR);
  CHECK(cfg);
  struct Bad { HeaderState st; const char *hdr; };
  const Bad bads[] = {
      {HeaderState::MISSING, ""},                                                        // unauthenticated
      {HeaderState::PRESENT, ""},                                                        // empty header
      {HeaderState::PRESENT, "Basic ZHVtcGVyOndyb25n"},                                  // dumper:wrong
      {HeaderState::PRESENT, "Basic ZHVtcGVyOmQzN2JhNzY0M2JjZDQwY2QxZjNmNzA2OTgyMGI1ODg"},  // one char short
      {HeaderState::PRESENT, "Basic ZHVtcGVyOmQzN2JhNzY0M2JjZDQwY2QxZjNmNzA2OTgyMGI1ODgxx"},  // one char long
      {HeaderState::PRESENT, "basic ZHVtcGVyOmQzN2JhNzY0M2JjZDQwY2QxZjNmNzA2OTgyMGI1ODgx"},  // scheme case
      {HeaderState::PRESENT, "Basic"},
      {HeaderState::TRUNCATED, GOOD_HDR},  // over-long header whose prefix is the right one
      {HeaderState::MISSING, GOOD_HDR},    // header bytes without a parsed header: still refused
  };
  for (const R &r : routes) {
    CHECK(route_for(r.m, r.uri) == r.route);
    for (const Bad &b : bads) {
      Served s = serve(r.m, r.uri, b.st, b.hdr, cfg, GOOD_HDR);
      CHECK(s.status == 401);
      CHECK(s.flash_reads == 0 && s.flash_bytes_sent == 0);
    }
    // The right login works, and only then is flash read.
    Served ok = serve(r.m, r.uri, HeaderState::PRESENT, GOOD_HDR, cfg, GOOD_HDR);
    CHECK(ok.status == 200 && ok.flash_reads == 1);
    // An unset or short login is never satisfied, not even by an empty or matching-looking header.
    for (const Bad &b : bads)
      CHECK(serve(r.m, r.uri, b.st, b.hdr, false, "").status == 401);
    CHECK(serve(r.m, r.uri, HeaderState::PRESENT, "", false, "").status == 401);
    CHECK(serve(r.m, r.uri, HeaderState::PRESENT, GOOD_HDR, false, GOOD_HDR).status == 401);
  }
  CHECK(!login_configured("", ""));
  CHECK(!login_configured("short-password", "Basic eDp5"));
  CHECK(!login_configured(GOOD_PW, "Basic "));
  CHECK(!login_configured(GOOD_PW, ""));
  CHECK(!header_matches("", ""));
  CHECK(!header_matches(nullptr, GOOD_HDR));

  // Path tricks are not routes (404, no flash), with or without a valid login; wrong methods neither.
  const char *const tricks[] = {"/dump/app/", "//dump/app", "/DUMP/APP", "/dump/App", "/%64ump/app", "%2Fdump%2Fapp",
                                "/dump/app%3Ftrim=0", "/dump/app/..", "/dump/./app", "/dump", "/dump/", "/",
                                "/dump/info/", "/dump/ota-unlock/", "/dump/infox", " /dump/app"};
  for (const char *t : tricks) {
    for (Method m : {Method::GET, Method::POST, Method::OTHER}) {
      CHECK(route_for(m, t) == Route::NONE);
      Served s = serve(m, t, HeaderState::PRESENT, GOOD_HDR, cfg, GOOD_HDR);
      CHECK(s.status == 404 && s.flash_reads == 0);
    }
  }
  CHECK(route_for(Method::POST, "/dump/app") == Route::NONE);       // HEAD/OPTIONS/POST on read routes:
  CHECK(route_for(Method::OTHER, "/dump/info") == Route::NONE);     // not registered, httpd answers 405
  CHECK(route_for(Method::GET, "/dump/ota-unlock") == Route::NONE);
}

// Every registered handler starts with the auth gate, and only the three gated handlers are registered.
static void test_handlers_gate_first() {
  const std::string cpp = slurp("components/flash_dumper/flash_dumper.cpp");
  for (const char *h : {"info_handler_", "app_handler_", "unlock_handler_"}) {
    const std::string sig = std::string("esp_err_t FlashDumper::") + h + "(httpd_req_t *req) {";
    const size_t at = cpp.find(sig);
    CHECK(at != std::string::npos);
    if (at == std::string::npos)
      continue;
    // The first two statements: fetch self, then the gate.
    const size_t body = at + sig.size();
    const size_t first_semicolon = cpp.find(';', body);
    const size_t gate = cpp.find("if (!self->authorized_(req))", body);
    CHECK(gate != std::string::npos && gate < cpp.find(';', first_semicolon + 1));
  }
  size_t count = 0;
  for (size_t at = cpp.find("httpd_register_uri_handler("); at != std::string::npos;
       at = cpp.find("httpd_register_uri_handler(", at + 1))
    count++;
  CHECK(count == 3);
  CHECK(cpp.find(".handler = info_handler_") != std::string::npos);
  CHECK(cpp.find(".handler = app_handler_") != std::string::npos);
  CHECK(cpp.find(".handler = unlock_handler_") != std::string::npos);
  CHECK(cpp.find("uri_match_fn") == std::string::npos);      // exact matching, no wildcard matcher
  CHECK(cpp.find("httpd_uri_match_wildcard") == std::string::npos);
  CHECK(cpp.find("if (!this->login_ok_)") != std::string::npos);  // refuses to start without a login
}

int main() {
  test_slot_selection();
  test_image_length();
  test_helpers();
  test_never_writes_flash();
  test_auth_every_route();
  test_handlers_gate_first();
  if (failures) {
    std::printf("test_flash_dumper: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("test_flash_dumper: all passed\n");
  return 0;
}
