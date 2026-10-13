// How much heap one HTTPS request needs before cloud_post.h lets it start. Kept free of ESPHome and
// ESP-IDF so the host test can check the arithmetic: tests/test_tls_budget.cpp.
//
// The client is ESPHome's http_request on ESP32: esp_http_client + esp-tls + mbedTLS 3.6 (ESP-IDF 5.5),
// TLS 1.2, sdkconfig CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN with IN 16384 / OUT 4096. MBEDTLS_CIPHER_MODE_CBC
// (always on in esp_config.h) and SHA-384 are enabled, and the build has no DTLS connection ID.
//
// The one big allocation is the record IN buffer, a single calloc in mbedtls_ssl_setup()
// (library/ssl_misc.h MBEDTLS_SSL_IN_BUFFER_LEN): header 13 + IV 16 + MAC 48 (SHA-384) + CBC padding 256
// + content 16384 = 16,717 B. It has to be one block, so the largest free block must hold it. 20 KB
// leaves ~3.7 KB for the allocator's own header and alignment, and for a small allocation from another
// task landing in that block between the check and the calloc. Everything else in a handshake is a
// few KB at most.
//
// The total, estimated, for a request to automyco.com (CloudFront, RSA-2048 chain):
//   task stack (cloud_post.h TASK_STACK, ESPHome's update_task)  8,192 + TCB ~400
//   IN record buffer                                             16,717
//   OUT record buffer (13 + 16 + 48 + 256 + 4096)                  4,429
//   ssl context, config, handshake params (SHA-256/384 states)     ~3,000
//   peer chain parsed (2-3 certs, DER copy + mbedtls_x509_crt)     ~5,500
//   CA from the certificate bundle, parsed while verifying        ~1,500
//   RSA / ECDHE bignum temporaries                                 ~2,000
//   esp_http_client (512 rx + 2048 tx buffers, handle, URL, headers)
//     and esp-tls                                                  ~3,500
//                                                          total  ~45,200
// (TX went from 512 to 2048 after 1.2.2, cloud-identity.yaml has why.)
// 48 KB total free leaves ~4 KB for Wi-Fi and lwIP buffers, the web server and the API while the
// handshake runs. Up to 1.2.2-rc1 the gate asked for 48 KB in ONE block. A heap with 114 KB free and a
// 31.7 KB largest block (the bench, 2026-10-11) never passed it, although TLS would have worked.
//
// A caller that keeps more alive during the session passes it as `extra_free`. The recipe fetch keeps
// an 8 KB download buffer plus the parsed JSON (cloud_recipes.h).
#pragma once

#include <cstddef>

namespace ts_cloud {

static constexpr size_t TLS_RECORD_OVERHEAD = 13 + 16 + 48 + 256;              // header + IV + MAC + padding
static constexpr size_t TLS_IN_BUFFER = TLS_RECORD_OVERHEAD + 16384;           // 16,717 B, one block
static constexpr size_t TLS_OUT_BUFFER = TLS_RECORD_OVERHEAD + 4096;           // 4,429 B
static constexpr size_t TLS_BLOCK_NEEDED = 20 * 1024;                          // largest free block
static constexpr size_t TLS_FREE_NEEDED = 48 * 1024;                           // total free

static_assert(TLS_IN_BUFFER + 2048 <= TLS_BLOCK_NEEDED, "the block gate must fit the IN buffer with slack");
static_assert(TLS_IN_BUFFER + TLS_OUT_BUFFER + 8192 < TLS_FREE_NEEDED, "the total gate must cover the big pieces");

// true if a heap with this largest block and this much free can take one TLS session plus `extra_free`.
inline bool tls_heap_ok(size_t largest_block, size_t free_total, size_t extra_free = 0) {
  return largest_block >= TLS_BLOCK_NEEDED && free_total >= TLS_FREE_NEEDED + extra_free;
}

}  // namespace ts_cloud
