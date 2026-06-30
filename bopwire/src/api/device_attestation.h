#pragma once
//
// Device attestation (#5, Axis-A anti-farming) — pluggable verifier.
//
// The play/mint rate limit is ~2,880 mints/device/day (realtime coverage
// gate, one stream per device). That bound is only meaningful if "device"
// is scarce and non-resettable. Today the player ships a random,
// SharedPreferences-cached `device_id` (forgeable). This interface lets a
// real hardware-attested verifier (Android Play Integrity + Keystore key
// attestation, iOS App Attest, Windows TPM/CNG) swap in WITHOUT touching
// any call site: the offline.play_proof / session.start hooks already call
// verify() and key the per-device limits on the returned device_id.
//
// The default `AcceptAllVerifier` derives a device_id from the attested
// `device_key` if present, else the legacy `device_id` field, so the
// per-device machinery is exercisable end-to-end NOW. Real verifiers (with
// pinned platform trust roots) live in a future device_attestation.cpp and
// are selected by config; the call sites never change.
//
// Wire shape of the optional `attestation` object on session.start / the
// offline bundle:
//   { "platform":"android|ios|windows", "level":"strongbox|tee|software",
//     "device_key":"<33B compressed pubkey or SPKI hash hex>",
//     "key_cert_chain":["<b64 DER>", ...],   // Keystore attestation chain
//     "integrity_token":"<Play Integrity JWS / App Attest blob>",
//     "nonce":"<server-issued challenge hex>",
//     "device_sig":"<device_key sig over the request canonical bytes>" }

#include "../crypto/hash.h"
#include "../crypto/keys.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace mc::api {

struct AttestationResult {
    bool        ok        = true;   // false → reject the request like a gate
    std::string device_id;          // canonical hex; "" if none could be derived
    std::string level     = "none"; // none | software | tee | strongbox
    std::string reason;             // populated on !ok
};

class DeviceAttestationVerifier {
public:
    virtual ~DeviceAttestationVerifier() = default;
    // `attestation` may be null/absent (old clients). `fallback_device_id`
    // is the legacy bundle device_id; `player_addr_hex` is the last-resort
    // key so the limiter still functions when no attestation is present.
    virtual AttestationResult verify(const nlohmann::json& attestation,
                                     const std::string& fallback_device_id,
                                     const std::string& player_addr_hex) = 0;
};

// Default: accept everything; derive a stable device_id so the per-device
// limiter is exercisable. Swap for a real verifier to make it Sybil-hard.
class AcceptAllVerifier final : public DeviceAttestationVerifier {
public:
    AttestationResult verify(const nlohmann::json& attestation,
                             const std::string& fallback_device_id,
                             const std::string& player_addr_hex) override {
        AttestationResult r;
        r.ok = true;
        std::string dk = attestation.is_object()
            ? attestation.value("device_key", std::string()) : std::string();
        if (!dk.empty()) {
            auto b = mc::crypto::from_hex(dk);
            Hash256 h = mc::crypto::sha256(b.data(), b.size());
            r.device_id = mc::crypto::to_hex(h);
            r.level     = attestation.value("level", std::string("software"));
        } else if (!fallback_device_id.empty()) {
            r.device_id = fallback_device_id;
            r.level     = "none";
        } else {
            r.device_id = player_addr_hex;   // last resort — per-wallet, not per-device
            r.level     = "none";
        }
        return r;
    }
};

} // namespace mc::api
