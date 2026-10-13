// Host tests for esphome/common/fw_version.h (cloud updates never go backwards).
//   cd esphome && g++ -std=c++17 -Wall -Wextra -Werror -I. -Icommon tests/test_fw_version.cpp -o /tmp/t && /tmp/t
#include "fw_version.h"

#include <cstdio>
#include <initializer_list>

using namespace fw_version;

static int failures = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                   \
    }                                                               \
  } while (0)

static void test_newer() {
  CHECK(is_newer("1.2.7", "1.2.6"));
  CHECK(is_newer("1.3.0", "1.2.6"));
  CHECK(is_newer("2.0.0", "1.99.99"));
  CHECK(is_newer("1.2.10", "1.2.9"));   // numeric, not string order
  CHECK(is_newer("1.10.0", "1.9.9"));
  CHECK(is_newer("1.3.0", "1.3.0-rc2"));   // the release after its pre-release
}

static void test_not_newer() {
  CHECK(!is_newer("1.2.6", "1.2.6"));   // reinstalling the same version: no
  CHECK(!is_newer("1.2.5", "1.2.6"));   // rollback: no (only by the owner, cloud-updates.yaml)
  CHECK(!is_newer("1.2.3", "1.2.6"));
  CHECK(!is_newer("1.2.9", "1.2.10"));
  CHECK(!is_newer("1.3.0-rc1", "1.3.0"));
  CHECK(!is_newer("1.3.0-rc1", "1.3.0-rc2"));   // pre-releases of one version: equal, so not newer
}

static void test_garbage_fails_closed() {
  for (const char *bad : {"", "1", "1.2", "1.2.x", "v1.2.7", "1.2.7 ", "1..2", "1.2.7-", "99999999.0.0", "1.2.65536",
                          "-1.2.3", "1.2.3.4", "1.2.7/../../x"}) {
    CHECK(!is_newer(bad, "1.2.6"));
    CHECK(!is_newer("1.2.7", bad));
  }
  CHECK(!is_newer(nullptr, "1.2.6"));
}

static void test_field_versions() {
  // Every chamber in the field (1.2.3-1.2.6) takes the next release and refuses the ones before it.
  for (const char *field : {"1.2.3", "1.2.4", "1.2.5", "1.2.6"}) {
    CHECK(is_newer("1.2.7", field));
    CHECK(!is_newer("1.2.2", field));
  }
}

int main() {
  test_newer();
  test_not_newer();
  test_garbage_fails_closed();
  test_field_versions();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("fw_version: all checks passed\n");
  return 0;
}
