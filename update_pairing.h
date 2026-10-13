#pragma once
// Public build only (terrashroom-public.yaml): the fingerprint the chamber publishes after update pairing.
//
// Hex of the first 8 bytes of SHA-256("automyco-ota-pair:" + password). The app computes the same value for
// the password it sent; a match proves the chamber stored *that* password, not one another device on the LAN
// sent first, and not just a window that timed out. The app's password is ~190 random bits, so 64 bits of
// its hash reveal nothing usable; the chamber still clears the value after 60 s in case someone set a weak
// password by hand. Keep in step with
// apple/AutoMyco/ShroomChamber/Firmware/UpdatePairing.swift (fingerprint(of:)).
#include <string>
#include "mbedtls/sha256.h"

inline std::string ts01_pair_fingerprint(const std::string &password) {
  const std::string input = "automyco-ota-pair:" + password;
  unsigned char digest[32];
  mbedtls_sha256(reinterpret_cast<const unsigned char *>(input.data()), input.size(), digest, 0);
  char hex[17];
  for (int i = 0; i < 8; i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
  return std::string(hex, 16);
}
