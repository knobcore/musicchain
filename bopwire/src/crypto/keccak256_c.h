#ifndef MC_KECCAK256_C_H
#define MC_KECCAK256_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Plain-C keccak256 used by the Android NDK wallet code (bopwire_android.c
 * is C, not C++). Bit-identical to mc::crypto::keccak256 — verified against
 * standard test vectors (keccak256("") starts c5d246..., keccak256("abc")
 * starts 4e0387...).
 *
 * out must have room for 32 bytes. */
void mc_keccak256(const uint8_t* data, size_t len, uint8_t out[32]);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MC_KECCAK256_C_H */
