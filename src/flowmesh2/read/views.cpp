// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/read/views.h>

#include <flowmesh2/ledger/state_commitment.h>

#include <flowmesh2/core/codec.h>
#include <flowmesh2/ledger/ledger.h>

#include <limits>
#include <utility>

namespace flowmesh2::read {

std::vector<unsigned char> CertifiedEnvelope::Encode() const
{
    std::vector<unsigned char> out{header.Encode()};
    const std::vector<unsigned char> cert_bytes{cert.Encode()};
    out.insert(out.end(), cert_bytes.begin(), cert_bytes.end());
    return out;
}

Result<CertifiedEnvelope> CertifiedEnvelope::Decode(std::span<const unsigned char> bytes)
{
    return DecodeExactWith(bytes, MAX_ENCODED_BYTES, [](SpanReader& reader) {
        CertifiedEnvelope envelope;
        reader >> envelope.header;
        reader >> envelope.cert.header_hash;
        ReadCappedVector(reader, envelope.cert.bitmap, MAX_BITMAP_BYTES);
        reader >> envelope.cert.aggregate;
        // A certified v3 head carries its COMMIT view after the aggregate.
        // Keep the legacy envelope framing exact: v0/v2 accept no extension.
        if (envelope.header.commitment_version == COMMITMENT_VERSION_VIEW_BOUND) {
            if (reader.size() != sizeof(uint64_t)) throw std::ios_base::failure("view-bound envelope requires its certificate view");
            uint64_t view{0};
            reader >> view;
            envelope.cert.view = view;
        }
        return envelope;
    });
}

ReadScope ScopeOf(const SlotHeaderV2& header)
{
    ReadScope scope;
    scope.domain = header.domain;
    scope.configuration = header.profile_hash;
    scope.validator_set = header.seat_set_root;
    scope.anchor = header.anchor.hash;
    scope.epoch = header.authority_epoch;
    scope.authority_revision = header.anchor.height;
    return scope;
}

CommitIdentity CommitOf(const SlotHeaderV2& header)
{
    CommitIdentity commit;
    commit.id = header.Hash();
    commit.parent = header.parent_hash;
    commit.state_root = header.state_root;
    commit.sequence = header.height;
    return commit;
}

SlotView::SlotView(CertifiedEnvelope envelope, const uint256& header_hash, StateRootsV0 roots, std::vector<OutcomeV2> outcomes,
                   MerkleTree outcome_tree, std::shared_ptr<const state::CoreState> state, std::map<ActionId, uint32_t> actions,
                   std::map<OrderId, std::vector<FillRef>> fills)
    : m_envelope{std::move(envelope)},
      m_header_hash{header_hash},
      m_roots{roots},
      m_outcomes{std::move(outcomes)},
      m_outcome_tree{std::move(outcome_tree)},
      m_state{std::move(state)},
      m_actions{std::move(actions)},
      m_fills{std::move(fills)}
{
}

Result<std::shared_ptr<const SlotView>> SlotView::Build(const state::HeadView& head, const SlotCertificate& cert,
                                                        std::vector<OutcomeV2> outcomes)
{
    const SlotHeaderV2& header{head.Header()};
    if (header.height == 0) return Error{ErrorCode::INVALID_INPUT, VIEW_GENESIS};
    if (cert.header_hash != head.HeaderHash()) return Error{ErrorCode::INVALID_INPUT, VIEW_CERT_HEADER};
    if (cert.Hash() != head.CertHash()) return Error{ErrorCode::INVALID_INPUT, VIEW_CERT_HASH};

    // Outcomes: their tree must be the certified one.
    if (outcomes.size() > std::numeric_limits<uint32_t>::max()) return Error{ErrorCode::INVARIANT, VIEW_INDEX_RANGE};
    std::vector<uint256> outcome_leaves;
    outcome_leaves.reserve(outcomes.size());
    for (const OutcomeV2& outcome : outcomes) outcome_leaves.push_back(outcome.LeafHash());
    MerkleTree outcome_tree{MerkleTree::Build(outcome_leaves)};
    if (outcomes.size() != header.outcome_count || outcome_tree.Root() != header.outcomes_root) {
        return Error{ErrorCode::INVALID_INPUT, VIEW_OUTCOMES};
    }

    // State: the head's state must commit to the header.
    const Result<StateRootsV0> roots{head.State().engine.Commit()};
    if (!roots.has_value()) return Error{ErrorCode::INVARIANT, roots.error().detail};
    if (roots->Root() != header.state_root) return Error{ErrorCode::INVARIANT, VIEW_STATE_ROOT};

    // The account proofs come from the ledger's own levels (M2-LED-02): the
    // view shares the head's state and copies no leaf.
    const Result<uint256> accounts_root{head.State().engine.ledger.AccountsRootV0()};
    if (!accounts_root.has_value()) return Error{ErrorCode::INVARIANT, accounts_root.error().detail};
    if (*accounts_root != roots->accounts) return Error{ErrorCode::INVARIANT, VIEW_ACCOUNTS_ROOT};

    // Indexes: actions by ActionId, fills by both orders.
    std::map<ActionId, uint32_t> actions;
    std::map<OrderId, std::vector<FillRef>> fills;
    for (uint32_t i{0}; i < outcomes.size(); ++i) {
        const OutcomeV2& outcome{outcomes[i]};
        if (outcome.source == OutcomeSource::ACTION && !actions.emplace(ActionId{outcome.subject}, i).second) {
            return Error{ErrorCode::INVARIANT, VIEW_ACTION_TWICE};
        }
        if (outcome.fills.size() > std::numeric_limits<uint32_t>::max()) return Error{ErrorCode::INVARIANT, VIEW_INDEX_RANGE};
        for (uint32_t j{0}; j < outcome.fills.size(); ++j) {
            fills[outcome.fills[j].maker].push_back(FillRef{i, j, FillRole::MAKER});
            fills[outcome.fills[j].taker].push_back(FillRef{i, j, FillRole::TAKER});
        }
    }

    return Ok(std::shared_ptr<const SlotView>{new SlotView{CertifiedEnvelope{header, cert}, head.HeaderHash(), *roots,
                                                           std::move(outcomes), std::move(outcome_tree), head.StatePtr(),
                                                           std::move(actions), std::move(fills)}});
}

Result<OutcomeWitness> SlotView::Outcome(uint32_t index) const
{
    if (index >= m_outcomes.size()) return Error{ErrorCode::INVALID_INPUT, "outcome index outside the slot"};
    Result<MerkleProof> proof{m_outcome_tree.Prove(index)};
    if (!proof.has_value()) return proof.error();
    return Ok(OutcomeWitness{index, m_outcomes[index], std::move(proof).value()});
}

std::optional<uint32_t> SlotView::ActionOutcome(const ActionId& id) const
{
    const auto it{m_actions.find(id)};
    if (it == m_actions.end()) return std::nullopt;
    return it->second;
}

std::span<const FillRef> SlotView::FillsOf(const OrderId& order) const
{
    const auto it{m_fills.find(order)};
    if (it == m_fills.end()) return std::span<const FillRef>{};
    return it->second;
}

std::optional<AccountWitness> SlotView::Account(const AccountId& account) const
{
    const ledger::LedgerState& ledger{m_state->engine.ledger};
    if (!ledger.HasAccount(account)) return std::nullopt;
    Result<ledger::AccountWitnessV0> witness{ledger::ProveAccountV0(ledger, account)};
    if (!witness.has_value()) return std::nullopt;
    return AccountWitness{std::move(witness->leaf), m_roots, std::move(witness->proof)};
}

} // namespace flowmesh2::read
