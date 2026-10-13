// Host tests for esphome/common/token_rotation.h (cloud token rotation, common/cloud-identity.yaml).
//   cd esphome && g++ -std=c++17 -Wall -Wextra -Werror -I. -Icommon tests/test_token_rotation.cpp -o /tmp/tst && /tmp/tst
//
// The main test cuts power at every point of a rotation (between any two flash writes, and with the
// server's answer lost) and checks that the chamber always ends up holding the token the server accepts.
#include "token_rotation.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace ts_rotation;

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      failures++;                                                       \
    }                                                                   \
  } while (0)

static std::string tok(char c) { return std::string(TOKEN_CHARS, c); }

// The two saved globals, as 65-byte buffers like std::array<char, 65>.
struct Slots {
  char token[TOKEN_CHARS + 1] = {};
  char next[TOKEN_CHARS + 1] = {};
  void set(char *dst, const std::string &v) {
    memset(dst, 0, TOKEN_CHARS + 1);
    memcpy(dst, v.data(), v.size() < TOKEN_CHARS ? v.size() : TOKEN_CHARS);
  }
};

// site/lambda/device-auth.mjs rotateDeviceToken, with tokens standing in for their hashes.
struct Server {
  std::string cur, prev;
  int rotate(const std::string &bearer, const std::string &next) {
    if (cur == next && prev == bearer) return 200;   // a retry of a rotation that already happened
    if (cur != bearer) return 401;
    prev = cur;
    cur = next;
    return 200;
  }
};

// One rotation as cloud-identity.yaml runs it, as a list of events. A power cut may come before any of
// them; everything in RAM is lost and the chamber boots from `flash`.
enum Event { FLASH_NEXT, SEND, ANSWER, FLASH_TOKEN, FLASH_CLEAR, DONE };

// Runs the protocol from boot until the chamber has nothing pending, cutting power before event `cut_at`
// on the first run (DONE = no cut). `lose_answer`: the server's answer never arrives on the first send.
// `wrong_order` models a broken firmware that clears `next` before saving the token.
static void run(Slots &flash, Server &srv, const std::string &fresh, int cut_at, bool lose_answer, bool wrong_order,
                bool &ok) {
  Slots ram = flash;
  bool armed = cut_at != DONE;   // the one power cut hasn't happened yet
  bool lossy = lose_answer;      // the first answer hasn't been lost yet
  bool started = false;
  for (int boots = 0; boots < 4; boots++) {
    if (boot_fixup(ram.token, ram.next)) memset(flash.next, 0, sizeof(flash.next));   // saved by the next flush
    if (!started) {   // the trigger (a pairing, a button press) happens once, before any cut
      ram.set(ram.next, fresh);
      started = true;
    }
    int ev = FLASH_NEXT;
    int status = 0;
    bool cut = false;
    while (ev != DONE) {
      if (armed && ev == cut_at) {
        armed = false;
        cut = true;
        break;
      }
      switch (ev) {
        case FLASH_NEXT:   // step 1 reaches flash before the request goes out
          if (!pending(ram.token, ram.next) && !committed(ram.token, ram.next)) { ev = DONE; continue; }
          flash.set(flash.next, ram.next);
          ev = committed(ram.token, ram.next) ? FLASH_TOKEN : SEND;
          break;
        case SEND:
          status = srv.rotate(ram.token, ram.next);
          if (lossy) status = -1;
          lossy = false;
          ev = ANSWER;
          break;
        case ANSWER:
          if (classify(status) == COMMIT) {
            ram.set(ram.token, ram.next);   // step 2 in RAM, `next` kept
            ev = wrong_order ? FLASH_CLEAR : FLASH_TOKEN;
          } else {
            ev = SEND;   // back off and retry (the server stays reachable in this model)
          }
          break;
        case FLASH_TOKEN:
          flash.set(flash.token, ram.token);
          ev = wrong_order ? DONE : FLASH_CLEAR;
          break;
        case FLASH_CLEAR:
          ram.next[0] = '\0';
          memset(flash.next, 0, sizeof(flash.next));
          ev = wrong_order ? FLASH_TOKEN : DONE;
          break;
      }
    }
    if (!cut) {
      ok = std::string(ram.token) == srv.cur && ram.next[0] == '\0';
      return;
    }
    // Power cut: what's on flash is all there is. It must still hold the server's token somewhere.
    if (std::string(flash.token) != srv.cur && std::string(flash.next) != srv.cur) {
      ok = false;
      return;
    }
    ram = flash;
  }
  ok = false;
}

static void test_reasons() {
  // docs/device-api.md "Rotation reasons": exact lowercase strings; only "reset" clears push subscriptions.
  CHECK(std::string(reason_str(REASON_RESET)) == "reset");
  CHECK(std::string(reason_str(REASON_REMOTE_PAIR)) == "remote_pair");
  CHECK(std::string(reason_str(REASON_UPDATE_PAIR)) == "update_pair");
  CHECK(reason_str(REASON_NONE) == nullptr);
  CHECK(reason_str(200) == nullptr);   // garbage from flash: no reason, a plain swap
  // The strongest pending reason wins: a pairing can't downgrade a queued reset, a reset upgrades a pairing.
  CHECK(merge_reason(REASON_RESET, REASON_UPDATE_PAIR) == REASON_RESET);
  CHECK(merge_reason(REASON_UPDATE_PAIR, REASON_RESET) == REASON_RESET);
  CHECK(merge_reason(REASON_NONE, REASON_REMOTE_PAIR) == REASON_REMOTE_PAIR);
  CHECK(merge_reason(REASON_REMOTE_PAIR, REASON_UPDATE_PAIR) == REASON_REMOTE_PAIR);
  CHECK(merge_reason(250, REASON_UPDATE_PAIR) == REASON_UPDATE_PAIR);
  CHECK(merge_reason(REASON_NONE, 99) == REASON_NONE);
  const std::string h(64, 'a');
  char body[128];
  CHECK(rotate_body(h.c_str(), REASON_RESET, body, sizeof(body)) == std::strlen(body));
  CHECK(std::string(body) == "{\"next_hash\":\"" + h + "\",\"reason\":\"reset\"}");
  rotate_body(h.c_str(), REASON_REMOTE_PAIR, body, sizeof(body));
  CHECK(std::string(body) == "{\"next_hash\":\"" + h + "\",\"reason\":\"remote_pair\"}");
  CHECK(std::strlen(body) <= 104);   // small: well inside the 2048-byte request buffer
  rotate_body(h.c_str(), REASON_NONE, body, sizeof(body));
  CHECK(std::string(body) == "{\"next_hash\":\"" + h + "\"}");   // byte for byte what 1.2.6 sent
}

int main() {
  test_reasons();
  // --- basics ---
  {
    Slots s;
    s.set(s.token, tok('a'));
    CHECK(!pending(s.token, s.next));
    CHECK(!committed(s.token, s.next));
    s.set(s.next, tok('b'));
    CHECK(pending(s.token, s.next));
    CHECK(!boot_fixup(s.token, s.next));   // a pending rotation survives a reboot
    CHECK(std::string(s.next) == tok('b'));
    s.set(s.token, tok('b'));
    CHECK(committed(s.token, s.next));
    CHECK(boot_fixup(s.token, s.next));    // step 3 lost: finished at boot
    CHECK(s.next[0] == '\0');
    s.set(s.next, "abc");                  // junk
    CHECK(!pending(s.token, s.next));
    CHECK(boot_fixup(s.token, s.next));
    CHECK(s.next[0] == '\0');
  }
  CHECK(classify(200) == COMMIT);
  CHECK(classify(204) == COMMIT);
  CHECK(classify(401) == START_OVER);
  CHECK(classify(429) == WAIT_RATE_LIMIT);
  CHECK(classify(400) == BACK_OFF);
  CHECK(classify(500) == BACK_OFF);
  CHECK(classify(-1) == BACK_OFF);

  // --- a power cut before every event, with and without a lost answer ---
  for (int cut = FLASH_NEXT; cut <= DONE; cut++) {
    for (int lose = 0; lose < 2; lose++) {
      Slots flash;
      flash.set(flash.token, tok('a'));
      Server srv{tok('a'), ""};
      bool ok = false;
      run(flash, srv, tok('b'), cut, lose, false, ok);
      if (!ok) std::printf("  cut before event %d, lost answer %d\n", cut, lose);
      CHECK(ok);
      // Once the new token is on flash, the rotation completes when the chamber is back up. A cut
      // before that (right after the trigger) loses the rotation, and nothing else: both sides keep 'a'.
      const std::string want = cut == FLASH_NEXT ? tok('a') : tok('b');
      CHECK(srv.cur == want);
      CHECK(std::string(flash.token) == want);
      CHECK(flash.next[0] == '\0');
    }
  }

  // --- the harness catches the hazard the step order exists for ---
  // Clearing `next` before the token reaches flash can leave the old token alone on flash after the
  // server swapped: the chamber would be locked out for good.
  {
    bool any_bad = false;
    for (int cut = FLASH_NEXT; cut <= DONE; cut++) {
      Slots flash;
      flash.set(flash.token, tok('a'));
      Server srv{tok('a'), ""};
      bool ok = false;
      run(flash, srv, tok('b'), cut, false, true, ok);
      if (!ok) any_bad = true;
    }
    CHECK(any_bad);
  }

  // --- someone with a copy of the old token rotates first: the chamber's attempt is refused (401) ---
  {
    Server srv{tok('a'), ""};
    CHECK(srv.rotate(tok('a'), tok('e')) == 200);   // the copy holder
    CHECK(classify(srv.rotate(tok('a'), tok('b'))) == START_OVER);
    CHECK(srv.cur == tok('e'));
  }

  if (failures == 0) std::printf("token_rotation: all passed\n");
  return failures == 0 ? 0 : 1;
}
