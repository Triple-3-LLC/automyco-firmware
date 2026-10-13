// Helper for common/push-alerts.yaml: an HTTPS POST that runs in its own FreeRTOS task, so the climate
// controller never waits on a TLS handshake (1-3 s on the ESP32) or a slow network.
//
// The task, the heap check and the one-request-at-a-time rule live in cloud_post.h, shared with the
// health report, remote access and the recipe fetch, so two cloud requests never start in the same loop
// iteration and fight over the heap. This file keeps the names push-alerts.yaml polls.
#pragma once

#include <atomic>
#include <string>

#include "cloud_post.h"

// 0 idle, 1 running, otherwise the HTTP status of the last request (-1 = no response: DNS, TLS or
// timeout). The consumer (push-alerts.yaml) resets it to 0 after reading a result.
static std::atomic<int> ts01_push_status{0};

// Starts the POST and returns true. Returns false if one is already running, another cloud request is in
// flight, or there isn't enough contiguous heap for a TLS session: try again later.
inline bool ts01_push_post_async(esphome::http_request::HttpRequestComponent *http, const std::string &url,
                                 const std::string &body, const char *device_id, const char *token) {
  return ts_cloud::post_async(http, url, body, device_id, token, ts01_push_status, "ts01_push");
}
