// The one place a cloud package starts an HTTPS request from: a FreeRTOS task on the shared cloud_http
// client (common/cloud-identity.yaml), so the climate controller and the heater's over-temperature
// cut-off never wait on a TLS handshake (1-3 s on the ESP32) or a slow network.
//
// Why one slot. push_alerts.h, fleet_health.h, common/remote_access.h and common/cloud_recipes.h each
// used to run their own task behind their own heap check. Three of them could pass that check in the
// same loop iteration and then fight over the heap mid-handshake. Now there is one in-flight slot for
// every cloud request: a package that finds it taken simply tries again on its own schedule (each keeps
// the retry/backoff behaviour it always had), and the heap check happens once, here, right before the
// task starts. What the check asks for, and why: common/tls_budget.h (a 20 KB block and 48 KB free).
//
// Each caller keeps its own status word (an std::atomic<int>): 0 idle, 1 running, otherwise the HTTP
// status of the finished request (-1 = no response: DNS, TLS or timeout). The caller resets it to 0 after
// reading a result. That is what the yaml packages poll, so their logic didn't change.
#pragma once

#include <atomic>
#include <string>
#include <utility>
#include <vector>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esphome/components/http_request/http_request.h"

#include "tls_budget.h"

namespace ts_cloud {

static constexpr uint32_t TASK_STACK = 8192;   // counted in tls_budget.h's total

// The heap right now, against common/tls_budget.h.
inline bool heap_ready(size_t extra_free = 0) {
  return tls_heap_ok(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), heap_caps_get_free_size(MALLOC_CAP_8BIT),
                     extra_free);
}

// true while any cloud request (POST or the recipe GET) is running.
inline std::atomic<bool> g_in_flight{false};

inline bool busy() { return g_in_flight.load(); }

// The firmware update check (common/cloud-updates.yaml) is ESPHome's own http_request update component,
// which runs its HTTPS GET in a FreeRTOS task named "update_task" instead of through this slot. It counts
// as a cloud request all the same: nothing here starts while it runs, and it only starts when
// tls_ready() (both sides are started from the main loop, so the check and the start can't interleave).
inline bool update_check_running() { return xTaskGetHandle("update_task") != nullptr; }

// Room for one more TLS session right now: nothing else in flight, and the heap passes tls_budget.h.
inline bool tls_ready(size_t extra_free = 0) {
  return !busy() && !update_check_running() && heap_ready(extra_free);
}

// Takes the in-flight slot if it's free and there is heap for a TLS session (plus `extra_free` the caller
// keeps during it). Callers that run their own task (cloud_recipes.h) call release() from that task when
// it is done.
inline bool acquire(size_t extra_free = 0) {
  if (update_check_running()) return false;
  if (!heap_ready(extra_free)) return false;
  bool expected = false;
  return g_in_flight.compare_exchange_strong(expected, true);
}

inline void release() { g_in_flight = false; }

static constexpr size_t MAX_REPLY = 1024;   // a reply body the caller asked to keep (remote access)

struct PostJob {
  esphome::http_request::HttpRequestComponent *http;
  std::string url;
  std::string body;
  std::vector<esphome::http_request::Header> headers;
  std::atomic<int> *status;
  std::string *reply = nullptr;   // filled with up to MAX_REPLY bytes of a 2xx body, before *status is set
};

inline void post_task(void *arg) {
  auto *job = static_cast<PostJob *>(arg);
  int status = -1;
  {
    auto c = job->http->post(job->url, job->body, job->headers);
    if (c != nullptr) {
      status = c->status_code;
      if (job->reply != nullptr) {
        job->reply->clear();
        if (status >= 200 && status < 300) {
          std::string buf(MAX_REPLY, '\0');
          size_t got = 0;
          uint32_t last = millis();
          while (got < MAX_REPLY) {
            int n = c->read(reinterpret_cast<uint8_t *>(&buf[got]), MAX_REPLY - got);
            auto r = esphome::http_request::http_read_loop_result(n, last, job->http->get_timeout(), c->is_read_complete());
            if (r == esphome::http_request::HttpReadLoopResult::RETRY) continue;
            if (r != esphome::http_request::HttpReadLoopResult::DATA) break;
            got += n;
          }
          buf.resize(got);
          *job->reply = std::move(buf);
        }
      }
      c->end();
    }
  }
  std::atomic<int> *out = job->status;
  delete job;
  *out = status;   // the caller's word first, so it can never see "idle" while the slot is still taken
  release();
  vTaskDelete(nullptr);
}

// Starts an authenticated JSON POST (X-Device-Id + Bearer token) and sets `status` to 1. Returns false,
// leaving `status` alone, if this caller already has one running, another cloud request holds the slot,
// there isn't heap for TLS, or the task couldn't be created: try again later.
inline bool post_async(esphome::http_request::HttpRequestComponent *http, const std::string &url, std::string body,
                       const char *device_id, const char *token, std::atomic<int> &status, const char *task_name,
                       std::string *reply = nullptr) {
  const int before = status.load();
  if (before == 1) return false;
  if (!acquire()) return false;
  auto *job = new PostJob{http, url, std::move(body),
                          {{"Content-Type", "application/json"},
                           {"X-Device-Id", device_id},
                           {"Authorization", std::string("Bearer ") + token}},
                          &status, reply};
  status = 1;
  if (xTaskCreate(post_task, task_name, TASK_STACK, job, 1, nullptr) != pdPASS) {
    delete job;
    status = before;
    release();
    return false;
  }
  return true;
}

}  // namespace ts_cloud
