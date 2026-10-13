#pragma once
// The humidify cycle's clock (common/terrashroom-core.yaml): where a cycle is, and whether the atomizer
// should be on right now.
//
// A cycle is Mist Burst seconds of atomizer time delivered as Mist Pulse On / Mist Pulse Off pulses with
// the blower at Mist Blower %, then 15 s of blower at 100 %, then rest until the cycle is 60 s old (longer
// only if the pulsed mist phase plus 20 s doesn't fit). The 1 s controller sets the cycle's start stamp and
// the phase; a 250 ms pulser switches the atomizer within the mist phase.
//
// The start is a millis() value stored | 1, so 0 can mean "no cycle". That makes a stamp taken on an even
// millisecond 1 ms *ahead* of now, and up to 1.2.2 the same tick computed now - start = 0xFFFFFFFF: the
// controller showed "cycle: resting" and the pulser held the atomizer off for the first second of every
// mist phase that started on an even millisecond (about half of them; one second of a 6-9 s mist phase).
// elapsed() reads a stamp up to a second ahead as 0.
//
// Plain C++ with no ESPHome types, so esphome/tests/test_mist_cycle.cpp can test it on a Mac.
#include <algorithm>
#include <cstdint>

namespace mist_cycle {

constexpr uint32_t BLOWER_MS = 15000;   // blower-only phase after the mist (stock: 15 s)
constexpr uint32_t REST_MIN_MS = 20000;   // a cycle is at least mist + 20 s ...
constexpr uint32_t CYCLE_MIN_MS = 60000;   // ... and at least 60 s (stock: 60 s)

// A millis() stamp that is never 0.
inline uint32_t stamp(uint32_t now) { return now | 1u; }

// Milliseconds since a stamp(), wrap-safe. A stamp up to a second ahead (the | 1 above) reads as 0.
inline uint32_t elapsed(uint32_t now, uint32_t since) {
  const uint32_t d = now - since;
  return d > 0xFFFFFFFFu - 1000u ? 0 : d;
}

struct Timing {
  uint32_t mist_ms;    // the pulsed mist phase, first pulse on to last pulse off
  uint32_t cycle_ms;   // the whole cycle
};

// burst_ms of atomizer time as on_ms pulses off_ms apart. off_ms == 0 is one continuous burst, the old
// behaviour; so is on_ms == 0 (pulse_on() never switches off then), which the UI's 0.5 s minimum rules out
// anyway. Up to 1.2.2 an on_ms of 0 gave a mist phase of 0 ms.
inline Timing timing(uint32_t burst_ms, uint32_t on_ms, uint32_t off_ms) {
  const uint32_t pulses = on_ms > 0 ? (burst_ms + on_ms - 1) / on_ms : 0;
  const uint32_t mist_ms = off_ms == 0 || pulses == 0 ? burst_ms : pulses * (on_ms + off_ms) - off_ms;
  return {mist_ms, std::max(CYCLE_MIN_MS, mist_ms + REST_MIN_MS)};
}

enum class Phase { MIST, BLOWER, REST };

// Where a cycle that started e ms ago is.
inline Phase phase(uint32_t e, const Timing &t) {
  if (e < t.mist_ms) return Phase::MIST;
  if (e < t.mist_ms + BLOWER_MS) return Phase::BLOWER;
  return Phase::REST;
}

// Whether the atomizer is on e ms into the mist phase: on for on_ms, off for off_ms, and so on.
inline bool pulse_on(uint32_t e, uint32_t on_ms, uint32_t off_ms) {
  if (off_ms == 0 || on_ms == 0) return true;
  return (e % (on_ms + off_ms)) < on_ms;
}

}  // namespace mist_cycle
