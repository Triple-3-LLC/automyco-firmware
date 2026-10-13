#pragma once
// Recipe and humidity-target rules shared by the climate controller (common/terrashroom-core.yaml), the push
// alerts (common/push-alerts.yaml) and the CO2 fresh-air guard (common/scd41.yaml): the built-in species
// presets, the humidity target in force per grow phase, and where a humidify cycle may start.
//
// Sources: docs/recipe-research.md (2026-10-12, recommendations 1-3) on top of the issue #30 audit in
// docs/species-presets.md. What changed and why is in docs/species-presets.md "Recipe fixes (2026-10-12)".
//
// Plain C++ with no ESPHome types, so esphome/tests/test_grow_climate.cpp can test it on a Mac.
#include <algorithm>
#include <cmath>
#include <cstring>

namespace grow_climate {

// The chamber's practical RH maximum: the soft ceiling holds misting at 95 % (released at 92 %), no cycle
// starts at >= 97 %, and the purge runs above 98 %. A target above 94 would just park the chamber on the
// soft ceiling, so no phase targets more than this.
constexpr float PINNING_RH_CAP = 94.0f;
// Up to 1.2.6 the pinning target was always setpoint + 3 (capped). That rule is kept for a grow with no
// known species (Custom picked by hand, or a cloud recipe whose name isn't a built-in species).
constexpr float LEGACY_PINNING_BOOST = 3.0f;

// A humidify cycle may start once RH is this far under the target, in Pinning and Fruiting alike. Stock used
// 5: one cycle lifts RH 3-5 %, so the chamber settled ~4 points under every target (measured in Pinning,
// LOG.md 2026-10-11). Pinning has used 2 since 1.2.3; Fruiting since the release after 1.2.6
// (recipe-research.md rec. 1).
// Why this can't short-cycle: a started cycle always runs to its end (at least 60 s, at least 20 s of it
// with the atomizer off, common/mist_cycle.h), the drying fans only start at target + 2 and stop at the
// target, so a 2-point gap separates "stop drying" from "start misting", and the soft ceiling (95/92) and
// the 97 % hard stop still bound the top.
constexpr float MIST_START_BELOW = 2.0f;
constexpr float NO_CYCLE_AT_OR_ABOVE = 97.0f;   // hard ceiling, any phase

enum class Phase { INCUBATION, PINNING, FRUITING };

inline Phase phase_from(const char *name) {
  if (name != nullptr && std::strcmp(name, "Incubation") == 0) return Phase::INCUBATION;
  if (name != nullptr && std::strcmp(name, "Pinning") == 0) return Phase::PINNING;
  return Phase::FRUITING;   // the select's default; an unknown phase runs the setpoints as they are
}

// Built-in Grow Preset values. Names must match the grow_preset select's options (and the app's Species.swift).
//   temp_f / rh   fruiting setpoints, applied only when the owner picks the preset (never at boot)
//   airflow       stock per-species idle fan duty X (analysis/stock-control-loop.md §7)
//   pinning_rh    absolute Pinning RH target (recipe-research.md rec. 2): 94 for most species, 92 for Lion's
//                 Mane and Black King, whose primordia references sit at 90-95 %
// Temperatures and RH follow the audited cloud recipes since the release after 1.2.6 (recipe-research.md rec. 3,
// species-presets.md): King Trumpet 60, Snow 60, Nameko 58, Blue 64 °F; RH Shiitake 85, Pink 85, Chestnut 90.
// Lion's Mane stays at 87 % (owner decision, recipe-research.md §3).
struct Preset {
  const char *name;
  float temp_f;
  float rh;
  int airflow;
  float pinning_rh;
};

constexpr Preset PRESETS[] = {
    {"Shiitake", 70, 85, 8, 94},        {"Lion's Mane", 70, 87, 11, 92},  {"Reishi", 72, 87, 7, 94},
    {"Pink Oyster", 75, 85, 10, 94},    {"Golden Oyster", 73, 91, 10, 94}, {"Blue Oyster", 64, 85, 10, 94},
    {"Chestnut", 70, 90, 9, 94},        {"King Trumpet", 60, 86, 10, 94}, {"Italian Oyster", 73, 89, 10, 94},
    {"Snow Oyster", 60, 90, 9, 94},     {"Black King", 70, 92, 9, 92},    {"Pioppino", 70, 93, 10, 94},
    {"Nameko", 58, 90, 8, 94},
};

inline const Preset *find_preset(const char *name) {
  if (name == nullptr) return nullptr;
  for (const auto &p : PRESETS)
    if (std::strcmp(p.name, name) == 0) return &p;
  return nullptr;
}

// The preset whose setpoints Grow Preset's on_value should write, or nullptr. At boot the select restores
// its saved option and fires on_value; that must never overwrite the restored (possibly hand-tuned)
// setpoints, so an OTA that changes this table leaves every running grow's setpoints as they were.
inline const Preset *preset_to_apply(const char *name, bool booted) { return booted ? find_preset(name) : nullptr; }

// A species' absolute pinning RH, or 0 for Custom / unknown names.
inline float species_pinning_rh(const char *name) {
  const Preset *p = find_preset(name);
  return p != nullptr ? p->pinning_rh : 0.0f;
}

// The species pinning RH in force: the selected preset's own value, or, with Grow Preset on Custom, the value
// kept when a setpoint hand edit or a cloud recipe switched it there (kept = 0: no species known).
inline float pinning_rh_for(const char *preset, float kept) {
  const float own = species_pinning_rh(preset);
  return own > 0 ? own : kept;
}

// The Pinning RH target.
//   Known species:  max(humidity setpoint, species pinning RH), capped at 94. The species value is a floor,
//                   so a setpoint the owner raised above it still wins, and a lower setpoint (Lion's Mane 87)
//                   can't drag pinning under what pins need.
//   No species:     setpoint + 3, capped at 94 (the rule up to 1.2.6).
inline float pinning_target(float setpoint, float species_rh) {
  if (std::isnan(setpoint)) return setpoint;
  if (species_rh > 0) return std::min(std::max(setpoint, species_rh), PINNING_RH_CAP);
  return std::min(setpoint + LEGACY_PINNING_BOOST, PINNING_RH_CAP);
}

// The humidity target the controller steers to. Incubation doesn't regulate RH (no misting, no drying);
// callers that need a number there get the setpoint.
inline float humidity_target(Phase phase, float setpoint, float species_rh) {
  return phase == Phase::PINNING ? pinning_target(setpoint, species_rh) : setpoint;
}

// Whether a new humidify cycle may start at RH h against target (the other interlocks: incubation, purge,
// soft ceiling, fresh air, low water, are the caller's).
inline bool mist_may_start(float h, float target) {
  return h <= target - MIST_START_BELOW && h < NO_CYCLE_AT_OR_ABOVE;
}

// The blower-only "topping up" band just under the target.
inline bool in_top_up_band(float h, float target) { return h > target - MIST_START_BELOW && h <= target; }

}  // namespace grow_climate
