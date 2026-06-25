#pragma once
#include "block.h"
#include <cstdint>
#include <vector>

namespace mc {

// Transaction type tags
enum class TxType : uint8_t {
    TRANSFER             = 0x01,
    MINT                 = 0x10,
    MODERATOR_OP         = 0x20,
    MODERATOR_PROPOSAL   = 0x30,
    USERNAME_REGISTER    = 0x40,
    SLASH                = 0x50,
    RELAY_REWARD         = 0x60,
};

// Sub-kinds of evidence carried inside a SlashTx. Two payload shapes for
// now — see consensus/slashing.h for the full design.
enum class SlashKind : uint8_t {
    EQUIVOCATION          = 1,  // same validator signed two height-N blocks
    FINGERPRINT_FORGERY   = 2,  // audio at content_hash doesn't match the
                                // declared fingerprint by chromaprint
};

// Moderator op codes (sub-type inside MODERATOR_OP).
//
// Moderator identity on chain is just (address, level, pubkey) — no
// human-readable handles are ever recorded.
//
// `TAG_LABEL_EDIT` is the catch-all for founder-level metadata changes:
// editing a song's ID3-style tags, defining a record label with its
// wallet splits, assigning an artist to a label, etc. The discriminator
// lives in the JSON payload's `"action"` field so we can extend the
// vocabulary without bumping the wire-format version every time.
enum class ModOpCode : uint8_t {
    GRANT          = 1,  // raise subject to `level`
    REVOKE         = 2,  // strip moderator status from subject
    TAG_LABEL_EDIT = 3,  // founder-only metadata edit (JSON-described)
};

// Moderator hierarchy levels.
//
//   FOUNDER (3) is the only level that can grant OP or REVOKE anyone,
//   and is set exactly once via the bootstrap self-grant. There is
//   exactly one FOUNDER per chain. The founder identifies by key
//   material only — there is no on-chain name for them.
//
//   OP (2) can propose hides / escrow releases and votes on proposals.
//   VOICE (1) is observer-only for now — placeholder for future
//   write-but-not-vote roles.
enum class ModLevel : uint8_t {
    NONE    = 0,
    VOICE   = 1,
    OP      = 2,
    FOUNDER = 3,
};

// ---- Transfer Transaction -------------------------------------------

struct TransferTx {
    Address  from_address;
    Address  to_address;
    uint64_t amount;       // internal units (8 decimals)
    uint64_t nonce;
    // Compressed secp256k1 pubkey of from_address, carried inline so
    // verify_signature can run without ECDSA public-key recovery (which
    // the OpenSSL-only crypto layer doesn't provide). Same pattern every
    // other signed tx type uses. The chain cross-checks
    // address_from_pubkey(from_pubkey) == from_address.
    PubKey33 from_pubkey{};
    Sig64    signature;

    std::vector<uint8_t> serialize() const;
    static bool deserialize(const uint8_t* data, size_t len, TransferTx& out);

    // Message bytes that are signed: chain_id|from|to|amount|nonce|from_pubkey
    std::vector<uint8_t> sign_message() const;
    Hash256 tx_hash() const;
    bool verify_signature() const;
};

// ---- Play Proof (embedded in MintTx) --------------------------------

struct PlayProof {
    Hash256  session_id;
    Hash256  content_hash;
    Hash256  block_hash;
    Address  artist_address;
    Address  player_address;
    Hash256  serving_node_id;
    uint64_t play_start_timestamp;
    uint64_t play_end_timestamp;
    uint32_t total_duration_ms;
    uint16_t heartbeat_count;
    Sig64    node_signature;

    std::vector<uint8_t> serialize() const;
    static bool deserialize(const uint8_t* data, size_t len, PlayProof& out);

    // Bytes over which node_signature is computed
    std::vector<uint8_t> sign_message() const;
};

// ---- Mint output ---------------------------------------------------

struct MintOutput {
    Address  recipient;
    uint64_t amount;
};

// ---- Mint Transaction ----------------------------------------------

struct MintTx {
    PlayProof               proof;
    std::vector<MintOutput> outputs;     // computed at processing time
    uint64_t                burn_amount = 0; // tokens burned from proof.player_address (post-50k)

    std::vector<uint8_t> serialize() const;
    static bool deserialize(const uint8_t* data, size_t len, MintTx& out);
    Hash256 tx_hash() const;
};

// ---- Moderator op transaction ---------------------------------------
//
// One transaction type covers every moderator-system mutation: granting
// or revoking moderator status. The op_code byte distinguishes which
// mutation is being made.
//
// Sign-and-verify covers everything between `op_code` and
// `proposer_pubkey` inclusive. Chain rules in chain.cpp enforce who is
// allowed to issue each op based on the proposer's current level.
//
// Deliberately no alias / handle / nickname field — moderator identity
// on chain is just (address, level, pubkey). Keeping names off the
// wire means a peer with a copy of the chain learns moderation power
// but never learns the operator's chosen handle.

struct ModeratorOpTx {
    uint8_t      op_code         = 0;     // ModOpCode value
    uint8_t      level           = 0;     // ModLevel value (0 for REVOKE)
    Address      subject{};               // who this op acts on (zero for TAG_LABEL_EDIT)
    PubKey33     subject_pubkey{};        // 33-byte compressed pubkey of subject (zero for REVOKE / TAG_LABEL_EDIT)
    Address      proposer{};              // who is issuing the op (== subject for self-bootstrap)
    PubKey33     proposer_pubkey{};       // proposer's pubkey for signature recovery
    uint64_t     nonce           = 0;     // per-proposer replay protection
    std::string  meta_json;               // JSON payload for TAG_LABEL_EDIT (empty for GRANT/REVOKE), max 4 KiB
    Sig64        signature{};             // ECDSA(sign_message())

    std::vector<uint8_t> serialize() const;
    static bool deserialize(const uint8_t* data, size_t len, ModeratorOpTx& out);

    std::vector<uint8_t> sign_message() const;
    Hash256              tx_hash() const;
    bool                 verify_signature() const;
};

// ---- Moderator proposal transaction --------------------------------
//
// Multi-step moderation actions go through this transaction type:
//
//   1. An OP (or the FOUNDER) builds a `ProposalKind::HIDE_CONTENT` or
//      `RELEASE_ESCROW` tx. The chain stores it as a pending proposal
//      and counts the proposer's own implicit YES vote.
//   2. Other OPs broadcast `ProposalKind::VOTE_YES` referencing the
//      proposal's tx-hash. Each YES decrements the remaining-needed
//      count.
//   3. When the YES count crosses the majority threshold of currently
//      active moderators (>floor(N/2)), the chain executes the action
//      atomically in the same block — the hide flag flips, or the
//      escrow balance moves to the artist.
//
// There is no NO vote and no expiry: silence is abstain, and a
// proposal sits in the pending table forever until quorum lands or the
// founder explicitly invalidates it (a future op_code can be added for
// that). Replay protection comes from the per-proposer nonce.
//
// The wire layout always carries the union of all action fields. Slots
// not relevant to a given kind must be sent as all-zero bytes — the
// chain rejects deserialized txs that have non-zero garbage in unused
// fields so the tx hash is canonical.

enum class ProposalKind : uint8_t {
    HIDE_CONTENT   = 1,   // hide a song by content_hash
    RELEASE_ESCROW = 2,   // release escrow from artist's escrow_address
    VOTE_YES       = 3,   // cast a YES vote on an existing proposal
};

struct ProposalTx {
    uint8_t   kind            = 0;      // ProposalKind

    // Per-kind payload (union, on the wire it's always all three).
    Hash256   target_hash{};            // HIDE: content_hash; VOTE_YES: prop_hash
    Address   target_addr{};            // RELEASE: artist address (else zero)
    uint64_t  amount          = 0;      // RELEASE: amount in internal units (else 0)

    Address   proposer{};               // voter / proposer (mlvl must be >= OP)
    PubKey33  proposer_pubkey{};
    uint64_t  nonce           = 0;      // per-proposer replay protection
    Sig64     signature{};              // ECDSA(sign_message())

    std::vector<uint8_t> serialize() const;
    static bool deserialize(const uint8_t* data, size_t len, ProposalTx& out);

    std::vector<uint8_t> sign_message() const;
    Hash256              tx_hash() const;
    bool                 verify_signature() const;
};

// ---- Username registration ------------------------------------------
//
// Any wallet can register a username (3..30 ASCII chars,
// `[a-z0-9_]+`, must start with a letter). First-come, first-served at
// the chain level. The username is purely a public lookup convenience:
// anyone resolving "lain" gets back the 20-byte address that registered
// it, which they can then verify cryptographically. It does NOT grant
// any login privilege on its own — proving ownership still requires the
// underlying secp256k1 key. The chain rejects re-registering a name
// that's already taken; a future "transfer" or "release" verb can be
// added to swap ownership.

struct UsernameTx {
    std::string name;                  // canonical, lowercased
    Address     owner{};               // wallet claiming the name
    PubKey33    owner_pubkey{};
    uint64_t    nonce         = 0;
    Sig64       signature{};

    std::vector<uint8_t> serialize() const;
    static bool deserialize(const uint8_t* data, size_t len, UsernameTx& out);

    std::vector<uint8_t> sign_message() const;
    Hash256              tx_hash() const;
    bool                 verify_signature() const;
};

// ---- Relay reward transaction ---------------------------------------
//
// Issued by a full node to credit a mini-node for relayed bytes. (#10)
// The amount is established by TRIANGULATION (RatsApi::try_corroborate):
// the broker brokered the stream, the mini-node signed a byte-report, and
// the player signed a byte-receipt; we credit min(relayed,received) bytes ×
// the per-byte rate. So `count` is now ALREADY internal token units (1 MC =
// 1e8 units), pre-computed by the relay tracker (1 MC per 10 MB ⇒ 10
// units/byte). One `RelayRewardTx` is emitted per (mini-node, sweep window).
//
// Wire format (all little-endian):
//   u8          TxType::RELAY_REWARD
//   Address(20) target_address     — mini-node wallet receiving the credit
//   u64         count              — INTERNAL UNITS to credit (1 MC = 1e8)
//   Address(20) issuer_address     — full node operator (founder)
//   PubKey33    issuer_pubkey
//   u64         nonce              — issuer's nonce
//   Sig64       signature          — issuer signs the preimage
//
// On apply, chain credits target_address by `count` units DIRECTLY (no
// per-MC scaling) and advances the issuer nonce. Without the founder
// signature the tx is rejected. (Phase-3 widens the issuer to "any
// validator"; the triangulation already makes each delivery single-issuer
// since only the broker holds the pending-delivery row.)
struct RelayRewardTx {
    Address  target_address{};
    uint64_t count          = 0;
    Address  issuer_address{};
    PubKey33 issuer_pubkey{};
    uint64_t nonce          = 0;
    Sig64    signature{};

    std::vector<uint8_t> serialize() const;
    static bool deserialize(const uint8_t* data, size_t len, RelayRewardTx& out);

    std::vector<uint8_t> sign_message() const;
    Hash256              tx_hash() const;
    bool                 verify_signature() const;
};

// ---- Slash transaction ----------------------------------------------
//
// Carries cryptographic evidence that some validator (target_address)
// cheated. Wire format (all little-endian):
//
//   u8                 SlashKind
//   Address(20)        target_address     — the validator being slashed
//   PubKey33           target_pubkey      — must hash to target_address
//   u32                evidence_len
//   bytes              evidence           — serialized {Equivocation,
//                                            FingerprintForgery}Proof
//   u64                nonce              — reporter's nonce
//   Address(20)        reporter_address   — who's filing the slash
//   PubKey33           reporter_pubkey
//   Sig64              signature          — over the sign_message preimage
//
// Chain::apply_slash verifies the cryptographic claim inside the
// evidence, then zeroes target_address's validator-confirmation weight
// going forward. Slashed addresses can still hold balance and submit
// transfers, but their Confirmation messages on future blocks no longer
// count toward the quorum.
struct SlashTx {
    SlashKind             kind;
    Address               target_address{};
    PubKey33              target_pubkey{};
    std::vector<uint8_t>  evidence;           // serialized proof
    uint64_t              nonce      = 0;
    Address               reporter_address{};
    PubKey33              reporter_pubkey{};
    Sig64                 signature{};

    std::vector<uint8_t> serialize() const;
    static bool deserialize(const uint8_t* data, size_t len, SlashTx& out);

    std::vector<uint8_t> sign_message() const;
    Hash256              tx_hash() const;
    bool                 verify_signature() const;
};

// ---- Generic transaction wrapper -----------------------------------

struct Transaction {
    TxType type;
    std::vector<uint8_t> raw; // raw serialized bytes including type byte

    Hash256 tx_hash() const;

    static Transaction from_transfer(const TransferTx& tx);
    static Transaction from_mint(const MintTx& tx);
    static Transaction from_moderator_op(const ModeratorOpTx& tx);
    static Transaction from_proposal(const ProposalTx& tx);

    bool parse_transfer(TransferTx& out) const;
    bool parse_mint(MintTx& out) const;
    bool parse_moderator_op(ModeratorOpTx& out) const;
    bool parse_proposal(ProposalTx& out) const;
};

} // namespace mc
