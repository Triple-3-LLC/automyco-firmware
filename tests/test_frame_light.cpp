// Host tests for lit timelapse frames (esphome/components/frame_uploader/frame_light.h): the OV2640
// exposure decode, the decide-to-light rule and the light -> settle -> discard -> capture -> release
// sequence. The property that matters most: whatever happens, the LED is never held past max_on_ms.
//   cd esphome && g++ -std=c++17 -Wall -Wextra -Werror -I. -Icommon tests/test_frame_light.cpp -o /tmp/tfl && /tmp/tfl
#include "components/frame_uploader/frame_light.h"

#include <cstdint>
#include <cstdio>

using namespace frame_light;

static int failures = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                   \
    }                                                               \
  } while (0)

static void test_gain() {
  CHECK(gain_x16(0x00) == 16);        // 1x
  CHECK(gain_x16(0x08) == 24);        // 1.5x
  CHECK(gain_x16(0x0F) == 31);        // 1.9375x
  CHECK(gain_x16(0x10) == 32);        // 2x (esp32_camera's default ceiling)
  CHECK(gain_x16(0x30) == 64);        // 4x
  CHECK(gain_x16(0xF0) == 256);       // 16x
  CHECK(gain_x16(0xFF) == 16 * 31);   // 31x, the maximum
}

static void test_aec() {
  CHECK(aec_lines(0, 0, 0) == 0);
  CHECK(aec_lines(0x03, 0x00, 0x00) == 3);              // AEC[1:0]
  CHECK(aec_lines(0x28, 0x00, 0x00) == 0);              // REG04's other bits (VREF/HREF) are ignored
  CHECK(aec_lines(0x00, 0xFF, 0x00) == 0x3FC);          // AEC[9:2]
  CHECK(aec_lines(0x00, 0x00, 0x01) == 0x400);          // AEC[15:10]
  CHECK(aec_lines(0x00, 0x00, 0xC0) == 0);              // REG45[7:6] are not exposure
  CHECK(aec_lines(0x02, 0x38, 0x01) == 0x400 + 0xE0 + 2);  // 1250 lines: a full UXGA frame
}

static void test_index() {
  CHECK(exposure_index(300, 0x00) == 300);    // lit chamber: short exposure, no gain
  CHECK(exposure_index(1248, 0x00) == 1248);  // shutter wide open at 1x
  CHECK(exposure_index(1248, 0x10) == 2496);  // ... and 2x gain: dark
  CHECK(exposure_index(0, 0xFF) == 0);
  CHECK(exposure_index(0xFFFF, 0xFF) == 0xFFFFu * 31u);  // no overflow at the extremes
}

static void test_decide() {
  const uint32_t T = 1000;
  CHECK(decide(true, false, true, 2496, T) == Decision::LIGHT);  // night
  CHECK(decide(true, false, true, 1000, T) == Decision::LIGHT);  // at the threshold
  CHECK(decide(true, false, true, 999, T) == Decision::PLAIN);   // just bright enough
  CHECK(decide(true, false, true, 300, T) == Decision::PLAIN);   // grow light on: natural frame
  CHECK(decide(false, false, true, 2496, T) == Decision::PLAIN); // switch off
  CHECK(decide(true, true, true, 2496, T) == Decision::PLAIN);   // LED already on (lit stream): leave it
  CHECK(decide(true, false, false, 0, T) == Decision::LIGHT);    // can't read the sensor: light
  CHECK(decide(false, false, false, 0, T) == Decision::PLAIN);   // ... unless switched off
}

// Simulated camera: a frame arrives frame_ms after each request. Returns how long the LED was held
// and whether a frame was used.
struct Run {
  uint32_t held_ms = 0;
  bool used = false;
  bool timed_out = false;
  int discarded = 0;
  uint32_t used_at = 0;
};

static Run simulate(uint32_t start, uint32_t frame_ms, bool camera_dead = false, Timing t = Timing()) {
  LitCapture lit(t);
  Run r;
  lit.begin(start);
  uint32_t now = start;
  int64_t frame_due = -1;  // relative ms; -1 = no request outstanding
  for (uint32_t step = 0; step < 10000 && lit.holding(); step++) {
    now = start + step;  // 1 ms loop
    const auto tk = lit.tick(now);
    if (tk == LitCapture::Tick::REQUEST)
      frame_due = step + frame_ms;
    if (tk == LitCapture::Tick::TIMEOUT)
      r.timed_out = true;
    if (!camera_dead && frame_due >= 0 && step == frame_due) {
      frame_due = -1;
      switch (lit.on_frame(now)) {
        case LitCapture::Frame::DISCARD:
          r.discarded++;
          frame_due = step + frame_ms;  // ask for the next one
          break;
        case LitCapture::Frame::USE:
          r.used = true;
          r.used_at = step;
          break;
        case LitCapture::Frame::EXPIRED:
          r.timed_out = true;
          break;
        case LitCapture::Frame::IGNORE:
          break;
      }
    }
    if (lit.holding())
      r.held_ms = step + 1;
  }
  CHECK(!lit.holding());  // every run ends released
  return r;
}

static void test_sequence_normal() {
  const Run r = simulate(1000, 150);  // ~150 ms per UXGA frame
  CHECK(r.used);
  CHECK(!r.timed_out);
  CHECK(r.discarded == 2);
  // settle 700 + 3 frames x 150 = 1150 ms of light: inside the 1-2 s budget.
  CHECK(r.used_at == 700 + 3 * 150);
  CHECK(r.held_ms <= 1200);
}

static void test_sequence_slow_camera() {
  const Run r = simulate(0, 500);  // 700 + 3 x 500 = 2200 ms: still under the cap
  CHECK(r.used);
  CHECK(r.held_ms <= 2500);
}

static void test_sequence_too_slow() {
  const Run r = simulate(0, 700);  // 700 + 3 x 700 = 2800 ms: over the cap, give up
  CHECK(!r.used);
  CHECK(r.timed_out);
  CHECK(r.held_ms <= 2500);
}

static void test_sequence_dead_camera() {
  const Run r = simulate(0, 150, true);  // no frame ever arrives
  CHECK(!r.used);
  CHECK(r.timed_out);
  CHECK(r.held_ms == 2500);
}

static void test_millis_wrap() {
  const Run r = simulate(0xFFFFFF00u, 150);  // the sequence straddles the 49.7-day millis() wrap
  CHECK(r.used);
  CHECK(r.held_ms <= 1200);
  const Run d = simulate(0xFFFFFF00u, 150, true);
  CHECK(d.timed_out);
  CHECK(d.held_ms == 2500);
}

static void test_frames_while_settling_are_ignored() {
  LitCapture lit;
  lit.begin(0);
  CHECK(lit.on_frame(100) == LitCapture::Frame::IGNORE);  // the camera's own idle frame, mid-ramp
  CHECK(lit.tick(100) == LitCapture::Tick::NONE);
  CHECK(lit.tick(700) == LitCapture::Tick::REQUEST);
  CHECK(lit.tick(701) == LitCapture::Tick::NONE);         // only one request
  CHECK(lit.on_frame(800) == LitCapture::Frame::DISCARD);
  CHECK(lit.on_frame(900) == LitCapture::Frame::DISCARD);
  CHECK(lit.on_frame(1000) == LitCapture::Frame::USE);
  CHECK(!lit.holding());
  CHECK(lit.on_frame(1100) == LitCapture::Frame::IGNORE);  // done: later frames aren't ours
  CHECK(lit.tick(5000) == LitCapture::Tick::NONE);
}

static void test_late_frame_expires() {
  LitCapture lit;
  lit.begin(0);
  CHECK(lit.tick(700) == LitCapture::Tick::REQUEST);
  // The loop stalled: no tick between 700 and 2600, then a frame shows up.
  CHECK(lit.on_frame(2600) == LitCapture::Frame::EXPIRED);
  CHECK(!lit.holding());
}

static void test_cancel_releases() {
  LitCapture lit;
  lit.begin(0);
  CHECK(lit.holding());
  lit.cancel();
  CHECK(!lit.holding());
  CHECK(lit.tick(100000) == LitCapture::Tick::NONE);
  CHECK(lit.on_frame(800) == LitCapture::Frame::IGNORE);
}

static void test_restart_after_use() {
  LitCapture lit;
  lit.begin(0);
  lit.tick(700);
  lit.on_frame(750);
  lit.on_frame(800);
  CHECK(lit.on_frame(850) == LitCapture::Frame::USE);
  // Next upload, 5 minutes later: a fresh sequence with a full discard count again.
  lit.begin(300000);
  CHECK(lit.holding());
  CHECK(lit.on_frame(300100) == LitCapture::Frame::IGNORE);
  CHECK(lit.tick(300700) == LitCapture::Tick::REQUEST);
  CHECK(lit.on_frame(300750) == LitCapture::Frame::DISCARD);
  CHECK(lit.on_frame(300800) == LitCapture::Frame::DISCARD);
  CHECK(lit.on_frame(300850) == LitCapture::Frame::USE);
}

static void test_default_budget() {
  const Timing t;
  CHECK(t.ramp_ms <= t.settle_ms);   // the fade finishes before frames are asked for
  CHECK(t.max_on_ms <= 2500);        // brief: not a photoperiod event
  CHECK(t.discard >= 1);             // the camera's queued frame always predates the light
}

int main() {
  test_gain();
  test_aec();
  test_index();
  test_decide();
  test_sequence_normal();
  test_sequence_slow_camera();
  test_sequence_too_slow();
  test_sequence_dead_camera();
  test_millis_wrap();
  test_frames_while_settling_are_ignored();
  test_late_frame_expires();
  test_cancel_releases();
  test_restart_after_use();
  test_default_budget();
  if (failures) {
    std::printf("test_frame_light: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("test_frame_light: all passed\n");
  return 0;
}
