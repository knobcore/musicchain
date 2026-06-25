#pragma once
//
// Hardware-derived device fingerprint (#5 structural attestation).
//
// Collects the host's STABLE hardware identifiers and returns the lowercase
// hex of their SHA-256. "Stable" = survives reboots and app reinstalls, so
// the full node's per-device rate limiter (≈2,880 mints/device/day) buckets
// real hardware instead of a resettable random token. This is the desktop
// (Windows / Linux / macOS) source; Android reports its own fingerprint over
// a Kotlin MethodChannel because the NDK can't read these identifiers.
//
// Identifiers folded in, best-effort (any that fail to read are skipped):
//   - primary non-loopback MAC address
//   - OS name + version string
//   - hostname / computer name
//   - Windows: HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid
//   - Linux:   /etc/machine-id, /sys/class/dmi/id/product_uuid (if readable)
//   - macOS:   IOPlatformUUID
//
// Deliberately NOT the wallet — device_id must be per-machine across wallets
// so a Sybil farmer can't mint a fresh device per wallet. The wallet binding
// happens one layer up (the attestation travels inside a wallet-signed
// bundle / session.start). Returns "" if NOTHING could be read, so the
// caller can fall back to a software-level random id.

#include <string>

namespace mc::util {

// Lowercase hex SHA-256 of the concatenated hardware identifiers, or "" if
// none could be read on this platform.
std::string device_fingerprint_hex();

} // namespace mc::util
