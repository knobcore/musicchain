#include "transaction.h"
#include "../crypto/hash.h"
#include "../crypto/signature.h"
#include <cstring>

namespace mc {

// ---- TransferTx -----------------------------------------------------

std::vector<uint8_t> TransferTx::sign_message() const {
    std::vector<uint8_t> msg;
    // EIP-155-style chain_id mixed in first so a TransferTx signature
    // never replays on Ethereum / BSC / Base / any other chain whose
    // own chain_id contributes to the sign hash.
    write_u32le(msg, MC_CHAIN_ID);
    msg.insert(msg.end(), from_address.begin(), from_address.end());
    msg.insert(msg.end(), to_address.begin(), to_address.end());
    write_u64le(msg, amount);
    write_u64le(msg, nonce);
    return msg;
}

std::vector<uint8_t> TransferTx::serialize() const {
    std::vector<uint8_t> buf;
    buf.push_back(static_cast<uint8_t>(TxType::TRANSFER));
    write_bytes(buf, from_address.data(), 20);
    write_bytes(buf, to_address.data(), 20);
    write_u64le(buf, amount);
    write_u64le(buf, nonce);
    write_bytes(buf, signature.data(), 64);
    return buf;
}

bool TransferTx::deserialize(const uint8_t* data, size_t len, TransferTx& out) {
    const uint8_t* p   = data;
    const uint8_t* end = data + len;
    // skip type byte
    if (p >= end || *p++ != static_cast<uint8_t>(TxType::TRANSFER)) return false;
    if (!read_bytes(p, end, out.from_address.data(), 20)) return false;
    if (!read_bytes(p, end, out.to_address.data(), 20)) return false;
    if (!read_u64le(p, end, out.amount)) return false;
    if (!read_u64le(p, end, out.nonce)) return false;
    if (!read_bytes(p, end, out.signature.data(), 64)) return false;
    return true;
}

Hash256 TransferTx::tx_hash() const {
    auto raw = serialize();
    return crypto::sha256(raw.data(), raw.size());
}

bool TransferTx::verify_signature() const {
    auto msg  = sign_message();
    auto hash = crypto::sha256(msg.data(), msg.size());
    // Derive pubkey from from_address for verification
    // For simplicity the from_address is derived as last 20 bytes of sha256(pubkey)
    // Full ECDSA verification is in crypto::verify_ecdsa
    return crypto::verify_ecdsa_from_address(hash, signature, from_address);
}

// ---- PlayProof ------------------------------------------------------

std::vector<uint8_t> PlayProof::sign_message() const {
    std::vector<uint8_t> msg;
    write_u32le(msg, MC_CHAIN_ID);
    write_bytes(msg, session_id.data(), 32);
    write_bytes(msg, content_hash.data(), 32);
    write_bytes(msg, block_hash.data(), 32);
    write_bytes(msg, artist_address.data(), 20);
    write_bytes(msg, player_address.data(), 20);
    write_bytes(msg, serving_node_id.data(), 32);
    write_u64le(msg, play_start_timestamp);
    write_u64le(msg, play_end_timestamp);
    write_u32le(msg, total_duration_ms);
    write_u16le(msg, heartbeat_count);
    return msg;
}

std::vector<uint8_t> PlayProof::serialize() const {
    std::vector<uint8_t> buf;
    write_bytes(buf, session_id.data(), 32);
    write_bytes(buf, content_hash.data(), 32);
    write_bytes(buf, block_hash.data(), 32);
    write_bytes(buf, artist_address.data(), 20);
    write_bytes(buf, player_address.data(), 20);
    write_bytes(buf, serving_node_id.data(), 32);
    write_u64le(buf, play_start_timestamp);
    write_u64le(buf, play_end_timestamp);
    write_u32le(buf, total_duration_ms);
    write_u16le(buf, heartbeat_count);
    write_bytes(buf, node_signature.data(), 64);
    return buf;
}

bool PlayProof::deserialize(const uint8_t* data, size_t len, PlayProof& out) {
    const uint8_t* p   = data;
    const uint8_t* end = data + len;
    if (!read_bytes(p, end, out.session_id.data(), 32)) return false;
    if (!read_bytes(p, end, out.content_hash.data(), 32)) return false;
    if (!read_bytes(p, end, out.block_hash.data(), 32)) return false;
    if (!read_bytes(p, end, out.artist_address.data(), 20)) return false;
    if (!read_bytes(p, end, out.player_address.data(), 20)) return false;
    if (!read_bytes(p, end, out.serving_node_id.data(), 32)) return false;
    if (!read_u64le(p, end, out.play_start_timestamp)) return false;
    if (!read_u64le(p, end, out.play_end_timestamp)) return false;
    if (!read_u32le(p, end, out.total_duration_ms)) return false;
    if (!read_u16le(p, end, out.heartbeat_count)) return false;
    if (!read_bytes(p, end, out.node_signature.data(), 64)) return false;
    return true;
}

// ---- MintTx ---------------------------------------------------------

std::vector<uint8_t> MintTx::serialize() const {
    std::vector<uint8_t> buf;
    buf.push_back(static_cast<uint8_t>(TxType::MINT));
    auto proof_bytes = proof.serialize();
    write_u32le(buf, static_cast<uint32_t>(proof_bytes.size()));
    buf.insert(buf.end(), proof_bytes.begin(), proof_bytes.end());
    write_u32le(buf, static_cast<uint32_t>(outputs.size()));
    for (const auto& o : outputs) {
        write_bytes(buf, o.recipient.data(), 20);
        write_u64le(buf, o.amount);
    }
    write_u64le(buf, burn_amount);
    return buf;
}

bool MintTx::deserialize(const uint8_t* data, size_t len, MintTx& out) {
    const uint8_t* p   = data;
    const uint8_t* end = data + len;
    if (p >= end || *p++ != static_cast<uint8_t>(TxType::MINT)) return false;
    uint32_t proof_len = 0;
    if (!read_u32le(p, end, proof_len)) return false;
    if (static_cast<size_t>(end - p) < proof_len) return false;
    if (!PlayProof::deserialize(p, proof_len, out.proof)) return false;
    p += proof_len;
    uint32_t out_count = 0;
    if (!read_u32le(p, end, out_count)) return false;
    out.outputs.resize(out_count);
    for (auto& o : out.outputs) {
        if (!read_bytes(p, end, o.recipient.data(), 20)) return false;
        if (!read_u64le(p, end, o.amount)) return false;
    }
    out.burn_amount = 0;
    if (p + 8 <= end) read_u64le(p, end, out.burn_amount); // optional field, backward compat
    return true;
}

Hash256 MintTx::tx_hash() const {
    auto raw = serialize();
    return crypto::sha256(raw.data(), raw.size());
}

// ---- ModeratorOpTx --------------------------------------------------

std::vector<uint8_t> ModeratorOpTx::sign_message() const {
    // Excludes `signature`. Bytes are laid out as in `serialize()`
    // minus the type byte and the trailing signature. Chain_id mixed
    // in first for cross-chain replay protection. The meta_json field
    // is length-prefixed (u16 LE) so a chain implementation that
    // doesn't know how to interpret the JSON can still skip past it
    // without parsing.
    std::vector<uint8_t> msg;
    write_u32le(msg, MC_CHAIN_ID);
    msg.push_back(op_code);
    msg.push_back(level);
    write_bytes(msg, subject.data(),         20);
    write_bytes(msg, subject_pubkey.data(),  33);
    write_bytes(msg, proposer.data(),        20);
    write_bytes(msg, proposer_pubkey.data(), 33);
    write_u64le(msg, nonce);
    write_u16le(msg, static_cast<uint16_t>(meta_json.size()));
    msg.insert(msg.end(), meta_json.begin(), meta_json.end());
    return msg;
}

std::vector<uint8_t> ModeratorOpTx::serialize() const {
    // EIP-155 convention: chain_id binds the signature but does NOT
    // appear in the transmitted bytes. Verifiers rebuild sign_message
    // on the receiving side using their own MC_CHAIN_ID and reject
    // mismatches.
    std::vector<uint8_t> buf;
    buf.push_back(static_cast<uint8_t>(TxType::MODERATOR_OP));
    buf.push_back(op_code);
    buf.push_back(level);
    write_bytes(buf, subject.data(),         20);
    write_bytes(buf, subject_pubkey.data(),  33);
    write_bytes(buf, proposer.data(),        20);
    write_bytes(buf, proposer_pubkey.data(), 33);
    write_u64le(buf, nonce);
    write_u16le(buf, static_cast<uint16_t>(meta_json.size()));
    buf.insert(buf.end(), meta_json.begin(), meta_json.end());
    write_bytes(buf, signature.data(), 64);
    return buf;
}

bool ModeratorOpTx::deserialize(const uint8_t* data, size_t len,
                                 ModeratorOpTx& out) {
    const uint8_t* p   = data;
    const uint8_t* end = data + len;
    if (p >= end || *p++ != static_cast<uint8_t>(TxType::MODERATOR_OP)) return false;
    if (p >= end) return false; out.op_code = *p++;
    if (p >= end) return false; out.level   = *p++;
    if (!read_bytes(p, end, out.subject.data(),         20)) return false;
    if (!read_bytes(p, end, out.subject_pubkey.data(),  33)) return false;
    if (!read_bytes(p, end, out.proposer.data(),        20)) return false;
    if (!read_bytes(p, end, out.proposer_pubkey.data(), 33)) return false;
    if (!read_u64le(p, end, out.nonce))                      return false;
    uint16_t meta_len = 0;
    if (!read_u16le(p, end, meta_len))                       return false;
    if (meta_len > 4096)                                     return false;
    if (static_cast<size_t>(end - p) < static_cast<size_t>(meta_len) + 64)
        return false;
    out.meta_json.assign(reinterpret_cast<const char*>(p), meta_len);
    p += meta_len;
    if (!read_bytes(p, end, out.signature.data(),       64)) return false;
    return true;
}

Hash256 ModeratorOpTx::tx_hash() const {
    auto raw = serialize();
    return crypto::sha256(raw.data(), raw.size());
}

bool ModeratorOpTx::verify_signature() const {
    auto  msg  = sign_message();
    auto  hash = crypto::sha256(msg.data(), msg.size());
    // The proposer's pubkey is carried inline (so the chain doesn't
    // need a separate registry just to verify), but we cross-check
    // that the pubkey actually hashes to the proposer address — that
    // closes the door on a malicious tx that signs against a pubkey
    // that isn't the proposer's.
    Address derived = crypto::address_from_pubkey(proposer_pubkey);
    if (std::memcmp(derived.data(), proposer.data(), 20) != 0) return false;
    return crypto::verify_ecdsa(hash, signature, proposer_pubkey);
}

// ---- ProposalTx -----------------------------------------------------

std::vector<uint8_t> ProposalTx::sign_message() const {
    // Wire layout (excluding type byte + signature):
    //   chain_id(4) | kind | target_hash(32) | target_addr(20)
    //        | amount(8) | proposer(20) | proposer_pubkey(33) | nonce(8)
    std::vector<uint8_t> msg;
    write_u32le(msg, MC_CHAIN_ID);
    msg.push_back(kind);
    write_bytes(msg, target_hash.data(),     32);
    write_bytes(msg, target_addr.data(),     20);
    write_u64le(msg, amount);
    write_bytes(msg, proposer.data(),        20);
    write_bytes(msg, proposer_pubkey.data(), 33);
    write_u64le(msg, nonce);
    return msg;
}

std::vector<uint8_t> ProposalTx::serialize() const {
    // EIP-155 convention: chain_id binds the signature but does NOT
    // appear in the transmitted bytes.
    std::vector<uint8_t> buf;
    buf.push_back(static_cast<uint8_t>(TxType::MODERATOR_PROPOSAL));
    buf.push_back(kind);
    write_bytes(buf, target_hash.data(),     32);
    write_bytes(buf, target_addr.data(),     20);
    write_u64le(buf, amount);
    write_bytes(buf, proposer.data(),        20);
    write_bytes(buf, proposer_pubkey.data(), 33);
    write_u64le(buf, nonce);
    write_bytes(buf, signature.data(), 64);
    return buf;
}

bool ProposalTx::deserialize(const uint8_t* data, size_t len,
                              ProposalTx& out) {
    const uint8_t* p   = data;
    const uint8_t* end = data + len;
    if (p >= end || *p++ != static_cast<uint8_t>(TxType::MODERATOR_PROPOSAL)) return false;
    if (p >= end) return false; out.kind = *p++;
    if (!read_bytes(p, end, out.target_hash.data(),     32)) return false;
    if (!read_bytes(p, end, out.target_addr.data(),     20)) return false;
    if (!read_u64le(p, end, out.amount))                     return false;
    if (!read_bytes(p, end, out.proposer.data(),        20)) return false;
    if (!read_bytes(p, end, out.proposer_pubkey.data(), 33)) return false;
    if (!read_u64le(p, end, out.nonce))                      return false;
    if (!read_bytes(p, end, out.signature.data(),       64)) return false;
    return true;
}

Hash256 ProposalTx::tx_hash() const {
    auto raw = serialize();
    return crypto::sha256(raw.data(), raw.size());
}

bool ProposalTx::verify_signature() const {
    auto msg  = sign_message();
    auto hash = crypto::sha256(msg.data(), msg.size());
    // Cross-check the inline pubkey against the proposer address —
    // otherwise a malicious tx could sign with a pubkey unrelated to
    // the address the chain will credit the vote against.
    Address derived = crypto::address_from_pubkey(proposer_pubkey);
    if (std::memcmp(derived.data(), proposer.data(), 20) != 0) return false;
    return crypto::verify_ecdsa(hash, signature, proposer_pubkey);
}

// ---- UsernameTx -----------------------------------------------------

std::vector<uint8_t> UsernameTx::sign_message() const {
    std::vector<uint8_t> msg;
    write_u32le(msg, MC_CHAIN_ID);
    msg.push_back(static_cast<uint8_t>(name.size()));
    msg.insert(msg.end(), name.begin(), name.end());
    write_bytes(msg, owner.data(),        20);
    write_bytes(msg, owner_pubkey.data(), 33);
    write_u64le(msg, nonce);
    return msg;
}

std::vector<uint8_t> UsernameTx::serialize() const {
    std::vector<uint8_t> buf;
    buf.push_back(static_cast<uint8_t>(TxType::USERNAME_REGISTER));
    buf.push_back(static_cast<uint8_t>(name.size()));
    buf.insert(buf.end(), name.begin(), name.end());
    write_bytes(buf, owner.data(),        20);
    write_bytes(buf, owner_pubkey.data(), 33);
    write_u64le(buf, nonce);
    write_bytes(buf, signature.data(),    64);
    return buf;
}

bool UsernameTx::deserialize(const uint8_t* data, size_t len,
                              UsernameTx& out) {
    const uint8_t* p   = data;
    const uint8_t* end = data + len;
    if (p >= end || *p++ != static_cast<uint8_t>(TxType::USERNAME_REGISTER)) return false;
    if (p >= end) return false;
    uint8_t name_len = *p++;
    if (name_len < 3 || name_len > 30) return false;
    if (static_cast<size_t>(end - p) < static_cast<size_t>(name_len) + 20 + 33 + 8 + 64)
        return false;
    out.name.assign(reinterpret_cast<const char*>(p), name_len);
    p += name_len;
    if (!read_bytes(p, end, out.owner.data(),        20)) return false;
    if (!read_bytes(p, end, out.owner_pubkey.data(), 33)) return false;
    if (!read_u64le(p, end, out.nonce))                   return false;
    if (!read_bytes(p, end, out.signature.data(),    64)) return false;
    return true;
}

Hash256 UsernameTx::tx_hash() const {
    auto raw = serialize();
    return crypto::sha256(raw.data(), raw.size());
}

bool UsernameTx::verify_signature() const {
    auto msg  = sign_message();
    auto hash = crypto::sha256(msg.data(), msg.size());
    Address derived = crypto::address_from_pubkey(owner_pubkey);
    if (std::memcmp(derived.data(), owner.data(), 20) != 0) return false;
    return crypto::verify_ecdsa(hash, signature, owner_pubkey);
}

// ---- SlashTx --------------------------------------------------------

std::vector<uint8_t> SlashTx::sign_message() const {
    std::vector<uint8_t> msg;
    write_u32le(msg, MC_CHAIN_ID);
    msg.push_back(static_cast<uint8_t>(kind));
    write_bytes(msg, target_address.data(),  20);
    write_bytes(msg, target_pubkey.data(),   33);
    write_u32le(msg, static_cast<uint32_t>(evidence.size()));
    write_bytes(msg, evidence.data(),         evidence.size());
    write_u64le(msg, nonce);
    write_bytes(msg, reporter_address.data(), 20);
    write_bytes(msg, reporter_pubkey.data(),  33);
    return msg;
}

std::vector<uint8_t> SlashTx::serialize() const {
    std::vector<uint8_t> buf;
    buf.push_back(static_cast<uint8_t>(TxType::SLASH));
    buf.push_back(static_cast<uint8_t>(kind));
    write_bytes(buf, target_address.data(),   20);
    write_bytes(buf, target_pubkey.data(),    33);
    write_u32le(buf, static_cast<uint32_t>(evidence.size()));
    write_bytes(buf, evidence.data(),          evidence.size());
    write_u64le(buf, nonce);
    write_bytes(buf, reporter_address.data(), 20);
    write_bytes(buf, reporter_pubkey.data(),  33);
    write_bytes(buf, signature.data(),         64);
    return buf;
}

bool SlashTx::deserialize(const uint8_t* data, size_t len, SlashTx& out) {
    const uint8_t* p   = data;
    const uint8_t* end = data + len;
    if (p >= end || *p++ != static_cast<uint8_t>(TxType::SLASH)) return false;
    if (p >= end) return false;
    out.kind = static_cast<SlashKind>(*p++);
    if (!read_bytes(p, end, out.target_address.data(), 20)) return false;
    if (!read_bytes(p, end, out.target_pubkey.data(),  33)) return false;
    uint32_t elen = 0;
    if (!read_u32le(p, end, elen)) return false;
    // Cap evidence at MAX_BLOCK_SIZE to prevent a malicious peer from
    // wedging memory with a 4 GiB length claim.
    if (elen > MAX_BLOCK_SIZE) return false;
    if (static_cast<size_t>(end - p) < elen + 8 + 20 + 33 + 64) return false;
    out.evidence.assign(p, p + elen);
    p += elen;
    if (!read_u64le(p, end, out.nonce))                       return false;
    if (!read_bytes(p, end, out.reporter_address.data(), 20)) return false;
    if (!read_bytes(p, end, out.reporter_pubkey.data(),  33)) return false;
    if (!read_bytes(p, end, out.signature.data(),         64)) return false;
    return true;
}

Hash256 SlashTx::tx_hash() const {
    auto raw = serialize();
    return crypto::sha256(raw.data(), raw.size());
}

bool SlashTx::verify_signature() const {
    auto msg  = sign_message();
    auto hash = crypto::sha256(msg.data(), msg.size());
    Address derived = crypto::address_from_pubkey(reporter_pubkey);
    if (std::memcmp(derived.data(), reporter_address.data(), 20) != 0)
        return false;
    return crypto::verify_ecdsa(hash, signature, reporter_pubkey);
}

// ---- Transaction wrapper -------------------------------------------

Transaction Transaction::from_transfer(const TransferTx& tx) {
    Transaction t;
    t.type = TxType::TRANSFER;
    t.raw  = tx.serialize();
    return t;
}

Transaction Transaction::from_mint(const MintTx& tx) {
    Transaction t;
    t.type = TxType::MINT;
    t.raw  = tx.serialize();
    return t;
}

Transaction Transaction::from_moderator_op(const ModeratorOpTx& tx) {
    Transaction t;
    t.type = TxType::MODERATOR_OP;
    t.raw  = tx.serialize();
    return t;
}

Transaction Transaction::from_proposal(const ProposalTx& tx) {
    Transaction t;
    t.type = TxType::MODERATOR_PROPOSAL;
    t.raw  = tx.serialize();
    return t;
}

bool Transaction::parse_transfer(TransferTx& out) const {
    return TransferTx::deserialize(raw.data(), raw.size(), out);
}

bool Transaction::parse_mint(MintTx& out) const {
    return MintTx::deserialize(raw.data(), raw.size(), out);
}

bool Transaction::parse_moderator_op(ModeratorOpTx& out) const {
    return ModeratorOpTx::deserialize(raw.data(), raw.size(), out);
}

bool Transaction::parse_proposal(ProposalTx& out) const {
    return ProposalTx::deserialize(raw.data(), raw.size(), out);
}

Hash256 Transaction::tx_hash() const {
    return crypto::sha256(raw.data(), raw.size());
}

} // namespace mc
