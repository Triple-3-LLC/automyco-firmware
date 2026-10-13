#pragma once
// Helpers for common/cloud-identity.yaml: what the chamber shows and sends about its cloud identity,
// derived from the two secrets it keeps (device ID, device token) so that neither is ever displayed.
// Server side: site/lambda/handle.mjs (keep the two in step).
//
//   public handle   first 12 hex of SHA-256(device ID). Shown as "Cloud Device ID". Can't be turned back
//                   into the ID, so a screenshot of the diagnostics page claims nothing.
//   pairing code    first 8 hex of HMAC-SHA256(key = hex SHA-256(token), message = decimal hour bucket,
//                   i.e. unix time / 3600). Shown as "Push Pairing Code", LAN only, rotating hourly. The
//                   iOS app sends it with the push registration; the server, which stores only the token's
//                   SHA-256, recomputes it. It proves the phone saw the chamber's LAN this hour.
//   hello backoff   the first-boot POST /api/device/hello that binds the token: 1, 2, 4 ... 60 min.
//   rotation        POST /api/device/rotate { next_hash }: the token is replaced after a pairing or a
//                   "Reset Cloud Identity" press (cloud-identity.yaml), with the same backoff.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

#include "esphome/core/helpers.h"

namespace ts_identity {

// 0 idle, 1 running, otherwise the HTTP status of the last hello (-1 = no response). cloud-identity.yaml
// resets it to 0 after reading a result. (cloud_post.h owns the task.)
inline std::atomic<int> g_hello_status{0};

// Same, for POST /api/device/rotate.
inline std::atomic<int> g_rotate_status{0};

// Goes up by one every time the token (or the whole identity) changes, so packages that derive something
// from the token (remote-access.yaml: the camera's frame key) know to derive it again.
inline std::atomic<uint32_t> g_token_gen{0};

// Same, for the "firmware update available" push that cloud-updates.yaml sends itself.
inline std::atomic<int> g_fw_push_status{0};

inline void to_hex(const uint8_t *in, size_t n, char *out) {
  for (size_t i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", in[i]);
}

// `bytes` random bytes as lowercase hex into out (2 * bytes + 1 chars). Call after Wi-Fi has started, when
// the ESP32 RNG is a real entropy source.
inline void fill_random_hex(char *out, size_t bytes) {
  uint8_t rnd[32];
  if (bytes > sizeof(rnd)) bytes = sizeof(rnd);
  esphome::random_bytes(rnd, bytes);
  to_hex(rnd, bytes, out);
  memset(rnd, 0, sizeof(rnd));
}

// out: 65 bytes.
inline void sha256_hex(const char *data, size_t n, char *out) {
  uint8_t digest[32];
  mbedtls_sha256(reinterpret_cast<const unsigned char *>(data), n, digest, 0);
  to_hex(digest, 32, out);
}

// out: 13 bytes.
inline void public_handle(const char *device_id, char *out) {
  char full[65];
  sha256_hex(device_id, strlen(device_id), full);
  memcpy(out, full, 12);
  out[12] = '\0';
}

// out: 9 bytes. false if mbedtls refused (never in practice).
inline bool pairing_code(const char *token, uint32_t hour_bucket, char *out) {
  char key[65];
  sha256_hex(token, strlen(token), key);
  char msg[16];
  const int n = snprintf(msg, sizeof(msg), "%lu", (unsigned long) hour_bucket);
  uint8_t mac[32];
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (md == nullptr || n <= 0 ||
      mbedtls_md_hmac(md, reinterpret_cast<const unsigned char *>(key), 64, reinterpret_cast<const unsigned char *>(msg), n, mac) != 0) {
    out[0] = '\0';
    return false;
  }
  to_hex(mac, 4, out);
  return true;
}

// Milliseconds to wait before hello attempt number `fails + 1`: 1, 2, 4, 8, 16, 32, 60, 60 ... minutes.
inline uint32_t hello_delay_ms(uint32_t fails) {
  uint32_t m = 1;
  for (uint32_t i = 0; i < fails && m < 60; i++) m *= 2;
  if (m > 60) m = 60;
  return m * 60000UL;
}

}  // namespace ts_identity
