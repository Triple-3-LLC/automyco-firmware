// Host tests for esphome/common/tls_budget.h (the heap cloud_post.h asks for before starting HTTPS).
//   c++ -std=c++17 -Wall -Wextra -Werror -I esphome/common esphome/tests/test_tls_budget.cpp -o /tmp/ttb && /tmp/ttb
#include "tls_budget.h"

#include <cstdio>

using namespace ts_cloud;

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      failures++;                                                       \
    }                                                                   \
  } while (0)

int main() {
  // mbedTLS 3.6 record buffers for IN 16384 / OUT 4096 with CBC and SHA-384 (library/ssl_misc.h).
  CHECK(TLS_IN_BUFFER == 16717);
  CHECK(TLS_OUT_BUFFER == 4429);

  // The bench, public 1.2.2-rc1 with saved Wi-Fi (2026-10-11): 114,016 B free, 31,744 B largest block.
  // The old 48 KB-in-one-block gate refused this; it has room for TLS.
  CHECK(tls_heap_ok(31744, 114016));
  CHECK(!(31744 >= 48 * 1024));   // what the old gate saw
  // The recipe fetch on the same heap (download buffer + JSON on top).
  CHECK(tls_heap_ok(31744, 114016, 8192 + 8 * 1024));

  // Public 1.2.0 (Bluetooth up): 29,952 B free. Still refused, as it should be.
  CHECK(!tls_heap_ok(29952, 29952));

  // Each half of the gate on its own.
  CHECK(!tls_heap_ok(TLS_BLOCK_NEEDED - 1, 200000));   // plenty free, no block for the IN buffer
  CHECK(!tls_heap_ok(100000, TLS_FREE_NEEDED - 1));    // one big block, not enough in total
  CHECK(tls_heap_ok(TLS_BLOCK_NEEDED, TLS_FREE_NEEDED));
  CHECK(!tls_heap_ok(TLS_BLOCK_NEEDED, TLS_FREE_NEEDED + 100, 101));
  CHECK(tls_heap_ok(TLS_BLOCK_NEEDED, TLS_FREE_NEEDED + 100, 100));

  // The block gate always holds the IN buffer.
  CHECK(TLS_BLOCK_NEEDED > TLS_IN_BUFFER);

  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("tls_budget: all passed\n");
  return 0;
}
