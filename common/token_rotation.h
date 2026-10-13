#pragma once
// Cloud token rotation decisions for common/cloud-identity.yaml, kept free of ESPHome and mbedtls so
// tests/test_token_rotation.cpp can check them on the host, power cuts included.
//
// The chamber holds two saved 65-byte strings: `token` (in use) and `next` (a new token waiting for the
// server's yes; empty when none). A rotation moves through three steps, each on flash before the next:
//   1. next := new token, then send SHA-256(next) authenticated with token
//   2. on a 2xx: token := next (next kept)
//   3. next := ""
// so flash never holds the old token without the new one. Server side: site/lambda/device-auth.mjs.

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ts_rotation {

constexpr size_t TOKEN_CHARS = 64;

inline bool valid(const char *s) { return strnlen(s, TOKEN_CHARS + 1) == TOKEN_CHARS; }

// A rotation is waiting for the server (step 1 done, step 2 not).
inline bool pending(const char *token, const char *next) {
  return valid(next) && strncmp(token, next, TOKEN_CHARS + 1) != 0;
}

// Step 2 is done but `next` hasn't been cleared yet (step 3 due).
inline bool committed(const char *token, const char *next) {
  return valid(next) && strncmp(token, next, TOKEN_CHARS + 1) == 0;
}

// At boot: a `next` that is junk or equal to the token (step 3 lost to a power cut) is cleared.
// Returns true if it cleared something.
inline bool boot_fixup(const char *token, char *next) {
  if (next[0] == '\0' || pending(token, next)) return false;
  memset(next, 0, TOKEN_CHARS + 1);
  return true;
}

enum Answer { COMMIT, START_OVER, WAIT_RATE_LIMIT, BACK_OFF };

// What the chamber does with the HTTP status of POST /api/device/rotate (-1 = no answer).
//   2xx  the server holds SHA-256(next): step 2
//   401  the token in use isn't this ID's any more (someone with a copy rotated first): new ID and token
//   429  at most one rotation per 10 min: keep `next`, try again in 10 min
//   else keep `next`, back off
inline Answer classify(int status) {
  if (status >= 200 && status < 300) return COMMIT;
  if (status == 401) return START_OVER;
  if (status == 429) return WAIT_RATE_LIMIT;
  return BACK_OFF;
}

// Why the chamber rotates, sent as "reason" on POST /api/device/rotate (docs/device-api.md "Rotation
// reasons"). Only "reset" makes the server do more (it also drops the ID's push subscriptions); a missing or
// unknown reason is a plain swap, as for firmware 1.2.0-1.2.6. Saved with the pending token
// (cloud_rotate_reason) so a replay after a lost answer sends the same reason. Several triggers before one
// rotation completes: the strongest wins (reset > remote_pair > update_pair), so a Reset Cloud Identity is
// never downgraded by a pairing that queued first.
enum Reason : uint8_t { REASON_NONE = 0, REASON_UPDATE_PAIR = 1, REASON_REMOTE_PAIR = 2, REASON_RESET = 3 };

inline uint8_t merge_reason(uint8_t pending_reason, uint8_t incoming) {
  if (incoming > REASON_RESET)
    incoming = REASON_NONE;
  if (pending_reason > REASON_RESET)
    pending_reason = REASON_NONE;
  return incoming > pending_reason ? incoming : pending_reason;
}

inline const char *reason_str(uint8_t r) {
  switch (r) {
    case REASON_RESET:
      return "reset";
    case REASON_REMOTE_PAIR:
      return "remote_pair";
    case REASON_UPDATE_PAIR:
      return "update_pair";
    default:
      return nullptr;
  }
}

// The request body: {"next_hash":"<64 hex>"} plus ,"reason":"..." when there is one. At most 104 bytes.
// `out` must hold 128.
inline size_t rotate_body(const char *next_hash, uint8_t reason, char *out, size_t cap) {
  const char *r = reason_str(reason);
  int n = r ? snprintf(out, cap, "{\"next_hash\":\"%.64s\",\"reason\":\"%s\"}", next_hash, r)
            : snprintf(out, cap, "{\"next_hash\":\"%.64s\"}", next_hash);
  return n < 0 ? 0 : (size_t) n;
}

}  // namespace ts_rotation
