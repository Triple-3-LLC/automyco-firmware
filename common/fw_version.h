#pragma once
// Anti-rollback for cloud updates (common/cloud-updates.yaml): the chamber only installs a release that is
// strictly newer than the one it runs. ESPHome's http_request update calls anything that differs from the
// running version "available", older included, so a server that is wrong (or not ours: DNS, a leaked
// signing key with an old, vulnerable but validly signed image) could otherwise walk the fleet backwards.
//
// Versions are "MAJOR.MINOR.PATCH" (esphome.project.version), each part 0-65535, optionally followed by
// "-<anything>" (a pre-release, which sorts before the plain release). Anything else doesn't parse, and an
// unparseable offer is never newer: fail closed.
//
// Pure C++17, no ESPHome: esphome/tests/test_fw_version.cpp.
#include <cstdint>
#include <cstring>

namespace fw_version {

struct Version {
  uint32_t part[3]{0, 0, 0};
  bool pre{false};   // "1.3.0-rc1": before 1.3.0
};

// Parses "1.2.6" or "1.3.0-rc1". False (and `out` unspecified) for anything else.
inline bool parse(const char *s, Version &out) {
  if (s == nullptr)
    return false;
  out = Version{};
  for (int i = 0; i < 3; i++) {
    if (*s < '0' || *s > '9')
      return false;
    uint32_t v = 0;
    int digits = 0;
    while (*s >= '0' && *s <= '9') {
      v = v * 10 + (uint32_t) (*s - '0');
      if (++digits > 5 || v > 65535)
        return false;
      s++;
    }
    out.part[i] = v;
    if (i < 2) {
      if (*s != '.')
        return false;
      s++;
    }
  }
  if (*s == '\0')
    return true;
  if (*s == '-' && s[1] != '\0') {
    out.pre = true;
    return true;
  }
  return false;
}

// -1, 0 or 1 like strcmp. Pre-releases of the same MAJOR.MINOR.PATCH compare equal to each other.
inline int compare(const Version &a, const Version &b) {
  for (int i = 0; i < 3; i++) {
    if (a.part[i] != b.part[i])
      return a.part[i] < b.part[i] ? -1 : 1;
  }
  if (a.pre != b.pre)
    return a.pre ? -1 : 1;
  return 0;
}

// True only if both parse and `offered` is strictly newer than `running`.
inline bool is_newer(const char *offered, const char *running) {
  Version o, r;
  return parse(offered, o) && parse(running, r) && compare(o, r) > 0;
}

}  // namespace fw_version
