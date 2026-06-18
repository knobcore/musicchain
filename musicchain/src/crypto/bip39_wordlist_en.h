#pragma once

namespace mc::crypto {

// Canonical BIP39 English wordlist, 2048 entries indexed 0..2047.
// Defined in bip39_wordlist_en.cpp (auto-generated from the upstream
// `bitcoin/bips` repo). Don't edit either file by hand — re-generate
// if the upstream list ever changes (it hasn't since 2013).
extern const char* const kBip39EnglishWordlist[2048];

} // namespace mc::crypto
