#pragma once
// flash_dumper's decisions, with no ESP-IDF or ESPHome dependencies so they run in host tests
// (esphome/tests/test_flash_dumper.cpp):
//   - pick_dump_slot(): which app partition to read (the one that is NOT running), or why not.
//   - parse_image():    how long the ESP32 app image at the start of a partition really is, read from
//                       its header and segment table (so a dump can be trimmed to the exact file the
//                       stock updater once wrote, which is what the known SHA-256 covers).
//
// Nothing here writes anything. The component only ever reads flash (see flash_dumper.cpp).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace dump_logic {

// ---- Slot selection -------------------------------------------------------------------------------

struct AppSlot {
  std::string label;  // "app0", "app1" (Arduino) or "ota_0", "ota_1" (IDF); "factory" is possible
  uint32_t address{0};
  uint32_t size{0};
};

enum class Pick {
  OK,
  NO_APPS,           // the partition table lists no app partitions (cannot happen on a booted chip)
  RUNNING_UNKNOWN,   // the running partition isn't in the list: refuse rather than guess
  NO_OTHER_SLOT,     // single-app table: there is no second slot, so nothing to dump
  AMBIGUOUS,         // 3+ app partitions and no obvious counterpart of the running one
};

struct Choice {
  Pick result{Pick::NO_APPS};
  int index{-1};    // into the apps vector, valid when result == OK
  int running{-1};  // index of the running slot, when known
};

inline const char *pick_str(Pick p) {
  switch (p) {
    case Pick::OK: return "ok";
    case Pick::NO_APPS: return "no app partitions";
    case Pick::RUNNING_UNKNOWN: return "running partition not found in the partition table";
    case Pick::NO_OTHER_SLOT: return "only one app partition: nothing to dump";
    case Pick::AMBIGUOUS: return "more than two app partitions: refusing to guess";
  }
  return "?";
}

// app0 <-> app1, ota_0 <-> ota_1. Empty when the label has no obvious counterpart.
inline std::string counterpart(const std::string &label) {
  if (label == "app0") return "app1";
  if (label == "app1") return "app0";
  if (label == "ota_0") return "ota_1";
  if (label == "ota_1") return "ota_0";
  return "";
}

// The partition to dump: the app slot the chip is NOT running. On a donor camera that has just taken
// the dumper through the stock updater, that is the slot still holding the stock image (the stock
// updater writes the inactive slot: "OTA Partition: app1", LOG.md 2026-10-09).
inline Choice pick_dump_slot(const std::vector<AppSlot> &apps, uint32_t running_address) {
  Choice c;
  if (apps.empty())
    return c;
  for (size_t i = 0; i < apps.size(); i++)
    if (apps[i].address == running_address)
      c.running = static_cast<int>(i);
  if (c.running < 0) {
    c.result = Pick::RUNNING_UNKNOWN;
    return c;
  }
  if (apps.size() == 1) {
    c.result = Pick::NO_OTHER_SLOT;
    return c;
  }
  if (apps.size() == 2) {
    c.index = c.running == 0 ? 1 : 0;
    c.result = Pick::OK;
    return c;
  }
  const std::string want = counterpart(apps[c.running].label);
  int found = -1;
  for (size_t i = 0; i < apps.size(); i++) {
    if (!want.empty() && apps[i].label == want) {
      if (found >= 0) {  // two partitions with the same label: don't pick either
        found = -1;
        break;
      }
      found = static_cast<int>(i);
    }
  }
  if (found < 0) {
    c.result = Pick::AMBIGUOUS;
    return c;
  }
  c.index = found;
  c.result = Pick::OK;
  return c;
}

// ---- ESP32 app image layout ------------------------------------------------------------------------
// esp_image_header_t (24 bytes): magic 0xE9, segment_count, spi_mode, spi_speed_size, entry_addr (4),
// wp_pin, spi_pin_drv[3], chip_id (2), min_chip_rev, min_chip_rev_full (2), max_chip_rev_full (2),
// reserved[4], hash_appended (offset 23). Then segment_count x {load_addr (4), data_len (4), data}.
// Then zero padding so the checksum byte is the last byte of a 16-byte block, the checksum byte, and,
// if hash_appended, a 32-byte SHA-256 of everything before it. This is what esptool elf2image writes,
// so an image cut to this length is byte-for-byte the .bin file that was flashed.

static constexpr uint8_t IMAGE_MAGIC = 0xE9;
static constexpr size_t HEADER_LEN = 24;
static constexpr size_t SEGMENT_HEADER_LEN = 8;
static constexpr size_t MAX_SEGMENTS = 16;  // ESP_IMAGE_MAX_SEGMENTS
static constexpr size_t DIGEST_LEN = 32;
// esp_app_desc_t sits at the start of the first segment's data: header + one segment header.
static constexpr size_t APP_DESC_OFFSET = HEADER_LEN + SEGMENT_HEADER_LEN;
static constexpr uint32_t APP_DESC_MAGIC = 0xABCD5432;

struct ImageInfo {
  bool ok{false};
  const char *error{"not parsed"};
  uint32_t length{0};          // whole image: segments + padding + checksum (+ appended digest)
  uint32_t digest_offset{0};   // where the appended SHA-256 starts (== length - 32) if hash_appended
  bool hash_appended{false};
  uint8_t segments{0};
  uint32_t entry{0};
  uint16_t chip_id{0};
};

inline uint32_t le32(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// End of the image after the segments at `pos`: pad to (16n - 1), then the checksum byte.
inline uint32_t checksum_end(uint32_t pos) { return (pos | 15u) + 1u; }

// read(offset, buf, len) -> bool reads from the start of the partition. Every read stays inside
// partition_size, and the result is rejected unless the whole image fits in the partition.
template<typename Read> ImageInfo parse_image(Read read, uint32_t partition_size) {
  ImageInfo info;
  uint8_t hdr[HEADER_LEN];
  if (partition_size < HEADER_LEN + 16 || !read(0, hdr, HEADER_LEN)) {
    info.error = "cannot read the image header";
    return info;
  }
  if (hdr[0] != IMAGE_MAGIC) {
    info.error = "no app image here (bad magic byte; empty or erased slot?)";
    return info;
  }
  info.segments = hdr[1];
  if (info.segments == 0 || info.segments > MAX_SEGMENTS) {
    info.error = "implausible segment count";
    return info;
  }
  info.entry = le32(hdr + 4);
  info.chip_id = uint16_t(hdr[12] | (hdr[13] << 8));
  if (hdr[23] > 1) {
    info.error = "implausible hash_appended flag";
    return info;
  }
  info.hash_appended = hdr[23] == 1;
  uint64_t pos = HEADER_LEN;
  for (uint8_t s = 0; s < info.segments; s++) {
    uint8_t seg[SEGMENT_HEADER_LEN];
    if (pos + SEGMENT_HEADER_LEN > partition_size || !read(static_cast<uint32_t>(pos), seg, SEGMENT_HEADER_LEN)) {
      info.error = "segment table runs past the partition";
      return info;
    }
    const uint32_t len = le32(seg + 4);
    if (len % 4 != 0) {
      info.error = "segment length not a multiple of 4";
      return info;
    }
    pos += SEGMENT_HEADER_LEN + uint64_t(len);
    if (pos > partition_size) {
      info.error = "segment runs past the partition";
      return info;
    }
  }
  uint64_t end = checksum_end(static_cast<uint32_t>(pos));
  if (info.hash_appended) {
    info.digest_offset = static_cast<uint32_t>(end);
    end += DIGEST_LEN;
  }
  if (end > partition_size) {
    info.error = "image is longer than the partition";
    return info;
  }
  info.length = static_cast<uint32_t>(end);
  info.ok = true;
  info.error = "";
  return info;
}

// Clamp a requested byte count to what /dump/app may serve: the trimmed image, or the whole slot.
inline uint32_t serve_length(const ImageInfo &info, uint32_t partition_size, bool trim) {
  if (!trim)
    return partition_size;
  return info.ok ? info.length : 0;  // refuse to "trim" an image we couldn't parse
}

// Lower-case hex of a digest.
inline std::string to_hex(const uint8_t *data, size_t len) {
  static const char *const HEX_DIGITS = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    out += HEX_DIGITS[data[i] >> 4];
    out += HEX_DIGITS[data[i] & 15];
  }
  return out;
}

// ---- Authentication and routing ---------------------------------------------------------------------
// Every request goes through plan_request() FIRST, before the handler reads flash, hashes, parses its
// query string or sends a single byte. Anything but a correct login gets 401 (with a fixed body that
// holds no flash data); a server whose login is unset or too short never answers anything else.

static constexpr size_t MIN_PASSWORD_LEN = 16;

// The expected header ("Basic " + base64) is usable only if a real password stands behind it.
inline bool login_configured(const std::string &password, const char *expected_header) {
  return password.size() >= MIN_PASSWORD_LEN && expected_header != nullptr && strlen(expected_header) > 6 &&
         strncmp(expected_header, "Basic ", 6) == 0;
}

// Constant-time compare of an Authorization header with the expected "Basic <base64>" value. An empty
// expected value never matches (an unset login must not be satisfied by an empty header).
inline bool header_matches(const char *got, const char *want) {
  if (got == nullptr || want == nullptr)
    return false;
  const size_t want_len = strlen(want);
  if (want_len == 0)
    return false;
  const size_t got_len = strlen(got);
  uint8_t diff = got_len != want_len;
  for (size_t i = 0; i < want_len; i++)
    diff |= static_cast<uint8_t>(want[i] ^ (i < got_len ? got[i] : 0));
  return diff == 0;
}

enum class Route { NONE, INFO, APP, UNLOCK };
enum class Method { GET, POST, OTHER };

// Exact, case-sensitive path match on the part before '?', the same rule esp_http_server applies to the
// registered URIs: "/dump/app/", "//dump/app", "/DUMP/app" and "%2Fdump%2Fapp" are not routes.
inline Route route_for(Method method, const std::string &uri) {
  const std::string path = uri.substr(0, uri.find('?'));
  if (method == Method::GET && path == "/dump/info") return Route::INFO;
  if (method == Method::GET && path == "/dump/app") return Route::APP;
  if (method == Method::POST && path == "/dump/ota-unlock") return Route::UNLOCK;
  return Route::NONE;
}

enum class HeaderState { MISSING, TRUNCATED, PRESENT };

struct Plan {
  int status;   // 401, 404 or 200 (= go ahead and run the route)
  Route route;
};

inline Plan plan_request(Method method, const std::string &uri, HeaderState state, const char *header,
                         bool configured, const char *expected) {
  const Route route = route_for(method, uri);
  if (route == Route::NONE)
    return {404, Route::NONE};
  const bool ok = configured && state == HeaderState::PRESENT && header_matches(header, expected);
  return ok ? Plan{200, route} : Plan{401, route};
}

// A printable, JSON-safe copy of a fixed-size char field from esp_app_desc_t (may lack a NUL).
inline std::string desc_field(const char *field, size_t size) {
  std::string out;
  for (size_t i = 0; i < size && field[i] != '\0'; i++) {
    const char c = field[i];
    out += (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') ? c : '?';
  }
  return out;
}

}  // namespace dump_logic
