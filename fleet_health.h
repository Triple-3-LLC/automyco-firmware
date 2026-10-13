// Helpers for common/fleet-health.yaml: a crash record that survives the reboot, the reset reason as
// text, and an HTTPS POST that runs in its own FreeRTOS task so the climate controller never waits on
// the network.
//
// Crash record. ESPHome's partition table has no coredump partition, and an OTA update can't add one
// (the table is only written by a serial install), so most chambers can't keep core dumps (those that
// kept the stock table can: see "core dump" below). For every chamber, this registers Arduino-ESP32's panic hook (set_arduino_panic_handler; the core already wraps
// ESP-IDF's esp_panic_handler and walks the backtrace for us). The hook copies the faulting core and up
// to 8 return addresses into RTC memory that a software reset doesn't clear; the normal panic output and
// reboot follow. The next boot reads it back. Addresses are resolved against the matching release's
// firmware.elf:  xtensa-esp32-elf-addr2line -pfiaC -e firmware.elf 0x400d1234 ...
// The hook only touches IRAM and RTC memory, because the flash cache may be off during a panic.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "esp_attr.h"
#include "esp_core_dump.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp32-hal.h"   // arduino_panic_info_t, set_arduino_panic_handler

#include "esphome/core/log.h"

#include "cloud_post.h"

static constexpr uint32_t TS01_CRASH_MAGIC = 0x7c01ca5e;
static constexpr int TS01_CRASH_DEPTH = 8;

struct ts01_crash_t {
  uint32_t magic;
  uint32_t check;   // magic ^ depth ^ pcs[0]: catches a half-written record
  uint32_t core;
  uint32_t depth;
  uint32_t pcs[TS01_CRASH_DEPTH];
};

inline const char *ts01_reset_reason();
inline void ts01_json_safe(char *dst, size_t n, const char *src);

RTC_NOINIT_ATTR static ts01_crash_t ts01_crash;

static void IRAM_ATTR ts01_panic_hook(arduino_panic_info_t *info, void *) {
  ts01_crash.magic = 0;
  ts01_crash.core = (uint32_t) info->core;
  uint32_t n = info->backtrace_len < TS01_CRASH_DEPTH ? info->backtrace_len : TS01_CRASH_DEPTH;
  if (n == 0 && info->pc != nullptr) {
    ts01_crash.pcs[0] = (uint32_t) info->pc;
    n = 1;
  } else {
    for (uint32_t i = 0; i < n; i++) ts01_crash.pcs[i] = info->backtrace[i];
  }
  ts01_crash.depth = n;
  ts01_crash.check = TS01_CRASH_MAGIC ^ n ^ ts01_crash.pcs[0];
  ts01_crash.magic = TS01_CRASH_MAGIC;
}

// Call once at boot.
inline void ts01_install_panic_hook() { set_arduino_panic_handler(ts01_panic_hook, nullptr); }

// The record from the crash that caused this boot, as JSON fields (`"crash_exc":"...","crash_pcs":"..."`),
// or "" if there isn't a valid one. Clears it, so each crash is read once.
inline std::string ts01_take_crash_json() {
  ts01_crash_t c = ts01_crash;
  ts01_crash.magic = 0;
  if (c.magic != TS01_CRASH_MAGIC || c.depth == 0 || c.depth > TS01_CRASH_DEPTH ||
      c.check != (TS01_CRASH_MAGIC ^ c.depth ^ c.pcs[0]))
    return "";
  // The exception kind is in the reset reason (panic, int_wdt, task_wdt), so it isn't stored here.
  const char *exc = ts01_reset_reason();
  std::string pcs;
  char buf[12];
  for (uint32_t i = 0; i < c.depth; i++) {
    snprintf(buf, sizeof(buf), "%s0x%08x", i ? " " : "", (unsigned) c.pcs[i]);
    pcs += buf;
  }
  char out[192];
  snprintf(out, sizeof(out), "\"crash_exc\":\"%s\",\"crash_core\":%u,\"crash_pcs\":\"%s\"", exc, (unsigned) c.core, pcs.c_str());
  return out;
}

// --- core dump (chambers that kept the stock partition table) ------------------------------------------
// Chambers moved over by OTA (stock BLE OTA or a stock-layout serial flash) still have the stock table,
// which has a 64 KB `coredump` partition at 0x3F0000. fleet-health.yaml turns on ESP-IDF's core dump to
// flash (ELF), so a panic or watchdog there leaves a full dump: every task's registers and stack. ESPHome's
// own table (web installer) has no such partition: ESP-IDF finds none and skips the dump.
// At boot this reads the dump's summary (task, exception cause, panic reason) once for the health report
// and the event log, and leaves the dump in flash for a serial pull (docs/fleet-updates.md "Core dumps").
// A new crash overwrites it. A dump that doesn't check out (a stock firmware leftover, or one cut off by a
// power loss) is erased once, so the next crash starts clean.

struct ts01_coredump_t {
  uint32_t id = 0;     // the dump's own checksum (last 4 bytes): tells one dump from the next; 0 = none
  std::string json;    // report fields: "crash_task":"...","crash_cause":N (only for a new dump)
  std::string event;   // event log line (only for a new dump)
};

// last_id: the id this returned on an earlier boot (saved), so each dump is reported once. rtc_json: this
// boot's RTC crash record; when it's missing, the dump's own backtrace fills crash_pcs/crash_exc.
inline ts01_coredump_t ts01_take_coredump(uint32_t last_id, const std::string &rtc_json) {
  ts01_coredump_t out;
  const esp_partition_t *part =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
  if (part == nullptr) return out;   // ESPHome's partition table: nothing to do
  uint32_t len = 0;
  if (esp_partition_read(part, 0, &len, sizeof(len)) != ESP_OK || len == 0xFFFFFFFF) return out;   // blank
  esp_core_dump_summary_t s;
  memset(&s, 0, sizeof(s));
  size_t addr = 0, size = 0;
  // ESP-IDF's summary reader parses the ELF without checking it is one, so check first that this is the
  // format this build writes: 24-byte header with version 0x0102 (ELF + CRC32, chip ESP32), then an ELF
  // header whose program headers lie inside the dump. A stock dump in another format would otherwise be
  // read as garbage offsets.
  uint8_t head[24 + 52];
  bool ours = len >= sizeof(head) + 4 && len <= part->size &&
              esp_partition_read(part, 0, head, sizeof(head)) == ESP_OK;
  if (ours) {
    uint32_t version, phoff;
    uint16_t phentsize, phnum;
    memcpy(&version, head + 4, 4);
    memcpy(&phoff, head + 24 + 28, 4);
    memcpy(&phentsize, head + 24 + 42, 2);
    memcpy(&phnum, head + 24 + 44, 2);
    ours = version == 0x0102 && memcmp(head + 24, "\x7f" "ELF", 4) == 0 && phentsize == 32 && phnum > 0 &&
           phoff < len && (uint64_t) phoff + (uint64_t) phnum * phentsize <= len - 24;
  }
  if (!ours || esp_core_dump_image_get(&addr, &size) != ESP_OK || esp_core_dump_image_check() != ESP_OK ||
      esp_core_dump_get_summary(&s) != ESP_OK ||
      esp_partition_read(part, size - sizeof(out.id), &out.id, sizeof(out.id)) != ESP_OK) {
    ESP_LOGW("crash", "core dump partition holds no readable dump (%u B header): erasing it", (unsigned) len);
    esp_core_dump_image_erase();
    out.id = 0;
    return out;
  }
  if (out.id == 0) out.id = 1;
  if (out.id == last_id) return out;   // reported on an earlier boot; still in flash for a serial pull
  char task[16];
  s.exc_task[sizeof(s.exc_task) - 1] = '\0';
  ts01_json_safe(task, sizeof(task), s.exc_task);
  if (task[0] == '\0') snprintf(task, sizeof(task), "?");
  // abort(), assert and watchdog panics carry a reason; a CPU exception has its cause register instead.
  char reason[112] = "";   // a watchdog lists the tasks that starved it
  const bool has_reason = esp_core_dump_get_panic_reason(reason, sizeof(reason)) == ESP_OK && reason[0];
  for (char *p = reason; *p; p++)
    if (*p == '\n' || *p == '\r') *p = ' ';
  const unsigned cause = s.ex_info.exc_cause & 0x3f;
  char buf[224];
  int n = snprintf(buf, sizeof(buf), "\"crash_task\":\"%s\"", task);
  if (!has_reason) n += snprintf(buf + n, sizeof(buf) - n, ",\"crash_cause\":%u", cause);
  out.json.assign(buf, n);
  if (rtc_json.empty() && s.exc_bt_info.depth > 0) {
    std::string pcs;
    char pc[12];
    const uint32_t depth = s.exc_bt_info.depth < TS01_CRASH_DEPTH ? s.exc_bt_info.depth : TS01_CRASH_DEPTH;
    for (uint32_t i = 0; i < depth; i++) {
      snprintf(pc, sizeof(pc), "%s0x%08x", i ? " " : "", (unsigned) s.exc_bt_info.bt[i]);
      pcs += pc;
    }
    out.json += ",\"crash_exc\":\"coredump\",\"crash_pcs\":\"" + pcs + "\"";
  }
  if (has_reason) {
    snprintf(buf, sizeof(buf), "crash dump (%u B): task %s, pc 0x%08x, %s", (unsigned) size, task,
             (unsigned) s.exc_pc, reason);
  } else {
    snprintf(buf, sizeof(buf), "crash dump (%u B): task %s, pc 0x%08x, exception cause %u", (unsigned) size,
             task, (unsigned) s.exc_pc, cause);
  }
  out.event = buf;
  return out;
}

inline const char *ts01_reset_reason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_EXT: return "ext";
    case ESP_RST_SW: return "sw";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    default: return "unknown";
  }
}

// True for resets that mean something went wrong (counted as crashes in the fleet dashboard).
inline bool ts01_reset_is_fault() {
  switch (esp_reset_reason()) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
      return true;
    default:
      return false;
  }
}

// Copies src into dst (size n) for use inside a JSON string: quotes, backslashes, control and
// non-ASCII bytes become '?'. Cloud recipe names (grow presets) can hold any of them.
inline void ts01_json_safe(char *dst, size_t n, const char *src) {
  size_t j = 0;
  for (size_t i = 0; src != nullptr && src[i] != '\0' && j + 1 < n; i++) {
    char c = src[i];
    dst[j++] = (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') ? c : '?';
  }
  dst[j] = '\0';
}

// --- background POST ---------------------------------------------------------
// The task itself is cloud_post.h (one cloud request at a time, shared heap check). ts01_post_status:
// 0 never run, 1 running, otherwise the HTTP status of the last request (-1 = no response: DNS, TLS or
// timeout). fleet-health.yaml resets it to 0 after reading a result.

static std::atomic<int> ts01_post_status{0};

// Starts the POST and returns true, or returns false if one is already running, another cloud request
// is in flight, or there isn't enough contiguous heap for a TLS session.
inline bool ts01_post_async(esphome::http_request::HttpRequestComponent *http, const std::string &url,
                            const std::string &body, const char *device_id, const char *token) {
  return ts_cloud::post_async(http, url, body, device_id, token, ts01_post_status, "ts01_post");
}
