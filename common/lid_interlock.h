#pragma once
// Lid reed switch as a UV-C interlock (common/lid-interlock.yaml, issue #28, docs/lid-interlock.md).
//
// The reed contact sits between the lid GPIO and GND with the internal pull-up on. Magnet near (lid shut)
// closes the contact: the pin reads LOW = closed. Anything else reads HIGH = OPEN: lid lifted, a broken or
// unplugged lead, a missing switch, a magnet that fell off. So every fault looks like an open lid, and an
// open lid means UV-C off.
//
// Asymmetric debounce: OPEN is believed at once (one HIGH reading cuts UV-C), CLOSED only after CLOSE_MS of
// uninterrupted LOW readings, so a lid being lowered, or a bouncing contact, can't briefly allow UV-C.
//
// This header decides; the package does the switching through two independent paths (the reed GPIO's own
// edge trigger and a 50 ms watchdog that reads the pin directly), and the core's UV-C Lamp turn-on refuses
// while uv_lid_ok is false. A hardware interlock (reed in series with the UV-C drive) is still the
// recommendation; see the doc.
//
// Plain C++ with no ESPHome types, so esphome/tests/test_lid_interlock.cpp can test it on a Mac. Times are
// millis() values; differences are taken unsigned, so the 49-day wrap is harmless.
#include <cstdint>

namespace lid_interlock {

constexpr uint32_t CLOSE_MS = 500;   // the lid must read closed this long before UV-C may turn on

// Pull-up + reed to GND: LOW = contact closed = lid shut. A floating pin (no switch) reads HIGH = open.
inline bool raw_closed(bool pin_high) { return !pin_high; }

struct Debounce {
  bool closed = false;      // debounced: starts OPEN, so UV-C is blocked until the lid proves it's shut
  bool seen_low = false;    // a LOW streak is running
  uint32_t low_since = 0;   // when it started
};

// Feeds one reading; returns the debounced "lid closed". Open is immediate.
inline bool update(Debounce &d, bool closed_now, uint32_t now) {
  if (!closed_now) {
    d.closed = false;
    d.seen_low = false;
    return false;
  }
  if (!d.seen_low) {
    d.seen_low = true;
    d.low_since = now;
  }
  if (!d.closed && (uint32_t) (now - d.low_since) >= CLOSE_MS) d.closed = true;
  return d.closed;
}

// Whether the lamp must be cut right now: it's on (the GPIO or the switch) and the lid isn't proven shut.
inline bool must_cut(bool uv_on, bool lid_closed) { return uv_on && !lid_closed; }

// Why a person's UV-C turn-on is refused, or nullptr if it may go ahead. Low water is checked first, as in
// the core (its message predates the lid). The core's turn-on handler implements the same rule inline.
inline const char *refusal(bool low_water, bool lid_ok) {
  if (low_water) return "UV-C blocked: reservoir low/empty";
  if (!lid_ok) return "UV-C blocked: lid open";
  return nullptr;
}

}  // namespace lid_interlock
