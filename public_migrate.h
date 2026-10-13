#pragma once
// Public build only (terrashroom-public.yaml): carry the owner's Wi-Fi across the stock -> ESPHome swap.
//
// The stock firmware keeps a plain copy of the owner's network in NVS namespace "credentials"
// (keys "ssid" / "password"; analysis/firmware_findings.md). Installing over Bluetooth only writes the
// spare app slot, so that NVS survives. Called once ever (terrashroom-public.yaml keeps the flag): if no
// network is saved, copy it into ESPHome's own Wi-Fi settings and reboot once so the normal connect path
// picks it up. If nothing is found, Wi-Fi comes over Improv as on any other public-build chamber.
#include <nvs.h>
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/components/wifi/wifi_component.h"

inline void ts01_import_stock_wifi() {
  auto *wifi = esphome::wifi::global_wifi_component;
  if (wifi == nullptr || wifi->has_sta()) return;   // a network is already saved or configured
  nvs_handle_t h;
  if (nvs_open("credentials", NVS_READONLY, &h) != ESP_OK) return;   // no stock settings on this unit
  char ssid[33] = {0}, pass[65] = {0};
  size_t sl = sizeof(ssid), pl = sizeof(pass);
  const bool found = nvs_get_str(h, "ssid", ssid, &sl) == ESP_OK && ssid[0] != '\0';
  if (found && nvs_get_str(h, "password", pass, &pl) != ESP_OK) pass[0] = '\0';
  nvs_close(h);
  if (!found) return;
  ESP_LOGI("migrate", "Imported the stock firmware's saved Wi-Fi; rebooting to join it");
  wifi->save_wifi_sta(ssid, pass);
  esphome::App.safe_reboot();
}
