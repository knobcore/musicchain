#include "slashing.h"
#include "../crypto/signature.h"

namespace mc {

bool EquivocationProof::verify() const {
    // Both confirmations must come from the same validator.
    if (conf_a.validator_id != conf_b.validator_id) return false;
    // The two blocks must actually be different.
    if (block_a_hash == block_b_hash) return false;
    // Both signatures must verify against the respective block hashes
    // with the validator's published pubkey.
    if (!crypto::verify_ecdsa(block_a_hash, conf_a.signature, conf_a.pubkey))
        return false;
    if (!crypto::verify_ecdsa(block_b_hash, conf_b.signature, conf_b.pubkey))
        return false;
    return true;
}

bool FingerprintForgeryProof::verify() const {
    // Cryptographic part is straightforward: the reporter's confirmation
    // signs the block_hash they're reporting on, so we can tell who's
    // raising the claim and that they really did so.
    if (!crypto::verify_ecdsa(block_hash,
                              reporter_conf.signature,
                              reporter_conf.pubkey)) return false;
    // The semantic part — "is the recomputed fingerprint actually
    // dissimilar from the declared one" — requires re-running chromaprint
    // on the audio at content_hash, which other validators do independently
    // before they sign the SlashTx that includes this proof. We don't
    // re-do that work inside .verify(); the apply-time check does.
    return similarity < 0.50f;  // matches DeepAuditor::kSlashThreshold
}

} // namespace mc
