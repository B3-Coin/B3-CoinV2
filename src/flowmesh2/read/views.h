// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_READ_VIEWS_H
#define B3_FLOWMESH2_READ_VIEWS_H

#include <flowmesh2/core/account_leaf.h>
#include <flowmesh2/core/certificate.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/merkle.h>
#include <flowmesh2/core/outcome.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/slot.h>
#include <flowmesh2/core/state_root.h>
#include <flowmesh2/read_slot.h>
#include <flowmesh2/state/candidate.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

/** Immutable read views of committed slots (slice R11; ARCHITECTURE.md
 * section 10.2).
 *
 * - CertifiedEnvelope: the header and certificate of one committed slot,
 *   the only payload the read service publishes into the C1 ReadSlot.
 *   Exact codec: the 366 header bytes, then the certificate's encoding.
 * - ScopeOf / CommitOf: the C1 ReadScope and CommitIdentity of a header
 *   (domain; configuration = profile_hash; validator_set = seat_set_root;
 *   anchor = anchor hash; epoch = authority_epoch; authority_revision =
 *   anchor height. id = header hash; parent; state_root; sequence =
 *   height).
 * - SlotView: everything a reply about one committed slot needs, built
 *   once by the publisher and never changed: the header and certificate,
 *   the outcomes with their tree, the v0 account leaves with their tree
 *   levels and the five component roots, an action index (ActionId to the
 *   outcome position) and an order-fill index (OrderId to every fill it
 *   took part in, as maker or as taker). Readers share it through
 *   shared_ptr<const SlotView>, so a reply's header, certificate, leaf and
 *   proof all come from one object.
 *
 * Not proof of anything by itself: a client checks every reply with
 * b3verify (verify/b3verify.h) against its trusted seat set (SEC-5).
 *
 * SlotView::Build costs O(state): it hashes every account leaf and keeps
 * every level of the accounts tree (REAL-REPLACE M2-LED-02: the sparse
 * Merkle tree of commitment v1 replaces the v0 levels with typed proofs
 * and O(changed) work per publish). Deterministic; no lock, clock or I/O.
 */
namespace flowmesh2::read {

// Static details (ErrorCode in parentheses).
inline constexpr std::string_view VIEW_GENESIS{"the genesis head has no certificate"};                //!< (INVALID_INPUT)
inline constexpr std::string_view VIEW_CERT_HEADER{"certificate does not name the head's header"};  //!< (INVALID_INPUT)
inline constexpr std::string_view VIEW_CERT_HASH{"certificate is not the one that decided the head"}; //!< (INVALID_INPUT)
inline constexpr std::string_view VIEW_OUTCOMES{"outcomes do not give the header's outcomes_root"};   //!< (INVALID_INPUT)
inline constexpr std::string_view VIEW_STATE_ROOT{"state does not give the header's state_root"};     //!< (INVARIANT)
inline constexpr std::string_view VIEW_ACCOUNTS_ROOT{"account leaves do not give the accounts root"}; //!< (INVARIANT)
inline constexpr std::string_view VIEW_ACTION_TWICE{"two outcomes for one action"};                   //!< (INVARIANT)
inline constexpr std::string_view VIEW_INDEX_RANGE{"more outcomes or fills than an index can hold"};  //!< (INVARIANT)

//! The header and certificate of one committed slot: the read-slot payload.
struct CertifiedEnvelope {
    SlotHeaderV2 header;
    SlotCertificate cert;

    static constexpr size_t MAX_ENCODED_BYTES{SLOT_HEADER_BYTES + SlotCertificate::MAX_ENCODED_BYTES};
    friend bool operator==(const CertifiedEnvelope&, const CertifiedEnvelope&) = default;

    //! header.Encode() || cert.Encode(). Total.
    std::vector<unsigned char> Encode() const;
    /** Exact decode: OVERSIZE above MAX_ENCODED_BYTES or for a bitmap above
     * its cap, DECODE for a malformed header or certificate or trailing
     * bytes. Pure codec: whether the certificate names the header and is
     * valid is b3verify's question (VerifyCertificate).
     */
    static Result<CertifiedEnvelope> Decode(std::span<const unsigned char> bytes);
};

//! The C1 scope of a slot header (see the file comment).
ReadScope ScopeOf(const SlotHeaderV2& header);
//! The C1 commit identity of a slot header (see the file comment).
CommitIdentity CommitOf(const SlotHeaderV2& header);

//! The side an order took in a fill. APPEND-ONLY; zero is never a value.
enum class FillRole : uint8_t {
    MAKER = 1,
    TAKER = 2,
};

//! One fill of an order in a slot: the outcome that records it, the fill's position in it, and the order's role.
struct FillRef {
    uint32_t outcome_index{0};
    uint32_t fill_index{0};
    FillRole role{FillRole::MAKER};
    friend bool operator==(const FillRef&, const FillRef&) = default;
};

//! A v0 account leaf, the five component roots and the leaf's proof under the accounts root.
struct AccountWitness {
    AccountLeafV0 leaf;
    StateRootsV0 roots;
    MerkleProof proof;
    friend bool operator==(const AccountWitness&, const AccountWitness&) = default;
};

//! A certified outcome, its position and its proof under the header's outcomes_root.
struct OutcomeWitness {
    uint32_t index{0};
    OutcomeV2 outcome;
    MerkleProof proof;
    friend bool operator==(const OutcomeWitness&, const OutcomeWitness&) = default;
};

/** The read view of one committed slot. Built only by Build, held only as
 * shared_ptr<const SlotView>.
 */
class SlotView
{
public:
    /** The view of the committed head `head`, decided by `cert`, whose slot
     * emitted `outcomes` (the candidate's outcomes, in emission order).
     * Refusals, in this order: INVALID_INPUT for the genesis head
     * (VIEW_GENESIS), a certificate that does not name the head's header
     * (VIEW_CERT_HEADER) or whose hash is not the head's certificate hash
     * (VIEW_CERT_HASH), and outcomes whose count and leaves do not give the
     * header's outcome_count and outcomes_root (VIEW_OUTCOMES); INVARIANT
     * when the head's state does not commit to the header's state_root
     * (VIEW_STATE_ROOT, or the commitment's own error), when the account
     * leaves do not give the accounts root (VIEW_ACCOUNTS_ROOT), for two
     * outcomes of one action (VIEW_ACTION_TWICE) and for more than 2^32 - 1
     * outcomes or fills (VIEW_INDEX_RANGE).
     */
    static Result<std::shared_ptr<const SlotView>> Build(const state::HeadView& head, const SlotCertificate& cert,
                                                         std::vector<OutcomeV2> outcomes);

    const SlotHeaderV2& Header() const { return m_envelope.header; }
    const uint256& HeaderHash() const { return m_header_hash; }
    const SlotCertificate& Certificate() const { return m_envelope.cert; }
    const CertifiedEnvelope& Envelope() const { return m_envelope; }
    const StateRootsV0& Roots() const { return m_roots; }

    //! The outcomes in emission order; their tree's root is the header's outcomes_root.
    const std::vector<OutcomeV2>& Outcomes() const { return m_outcomes; }
    //! Outcome `index` with its proof; INVALID_INPUT for an index outside the slot.
    Result<OutcomeWitness> Outcome(uint32_t index) const;
    //! The outcome position of the action `id` in this slot, if the slot executed it.
    std::optional<uint32_t> ActionOutcome(const ActionId& id) const;
    //! Every fill of `order` in this slot, in outcome then fill order (empty if none).
    std::span<const FillRef> FillsOf(const OrderId& order) const;

    //! The number of accounts (v0 leaves).
    size_t AccountCount() const { return m_accounts.size(); }
    //! The leaf of `account`, its proof and the roots; nullopt if the state has no such account (v0 has no absence proof).
    std::optional<AccountWitness> Account(const AccountId& account) const;

private:
    SlotView(CertifiedEnvelope envelope, const uint256& header_hash, StateRootsV0 roots, std::vector<OutcomeV2> outcomes,
             MerkleTree outcome_tree, std::vector<AccountLeafV0> accounts, MerkleTree account_tree,
             std::map<ActionId, uint32_t> actions, std::map<OrderId, std::vector<FillRef>> fills);

    CertifiedEnvelope m_envelope;
    uint256 m_header_hash;
    StateRootsV0 m_roots;
    std::vector<OutcomeV2> m_outcomes;
    MerkleTree m_outcome_tree;
    std::vector<AccountLeafV0> m_accounts; //!< ascending AccountId, the accounts tree's leaf order
    MerkleTree m_account_tree;              //!< REAL-REPLACE M2-LED-02 (v0 tree levels)
    std::map<ActionId, uint32_t> m_actions;
    std::map<OrderId, std::vector<FillRef>> m_fills;
};

} // namespace flowmesh2::read

#endif // B3_FLOWMESH2_READ_VIEWS_H
