// Writes restored globals to flash now, for values a power cut must not lose (a password, the cloud token).
//
// A `restore_value: true` global (ESPHome 2026.9 RestoringGlobalsComponent) only hands its value to the
// flash queue on its own 1 s poll, update(), or at shutdown. So setting one and calling
// global_preferences->sync() straight after writes the OLD value, and the new one waits for the next 60 s
// flush. Running update() first queues the new value, then sync() commits it.
//
// Takes the global's component (the bare ID, not id(x)):  ts_persist_now(cloud_device_token, cloud_bound);
#pragma once

#include "esphome/core/preferences.h"

template<typename... G> inline void ts_persist_now(G *...globals) {
  (globals->update(), ...);
  esphome::global_preferences->sync();
}
