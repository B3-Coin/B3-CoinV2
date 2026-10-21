// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/state/candidate.h>

#include <flowmesh2/core/merkle.h>
#include <flowmesh2/core/state_root.h>

#include <cstddef>
#include <limits>
#include <utility>

namespace flowmesh2::state {

namespace {

constexpr Error Invalid(std::string_view detail) { return Error{ErrorCode::INVALID_INPUT, detail}; }

} // namespace

Result<CoreState> CoreState::Genesis(const ProfileParams& profile, std::span<const MarketDescriptor> markets,
                                     const uint256& seat_set_root)
{
    if (seat_set_root.IsNull()) return Invalid(GENESIS_ZERO_SEAT_SET);
    Result<engine::EngineState> state{engine::EngineState::Genesis(profile, markets)};
    if (!state.has_value()) return state.error();
    return Ok(CoreState{std::move(state).value(), seat_set_root});
}

HeadView::HeadView(const SlotHeaderV2& header, const uint256& cert_hash, std::shared_ptr<const CoreState> state)
    : m_header{header}, m_header_hash{header.Hash()}, m_cert_hash{cert_hash}, m_state{std::move(state)}
{
}

Result<HeadView> HeadView::Make(const SlotHeaderV2& header, const uint256& cert_hash, std::shared_ptr<const CoreState> state)
{
    if (!state) return Invalid(HEAD_STATE_MISSING);
    return Ok(HeadView{header, cert_hash, std::move(state)});
}

ParentRef ParentRef::Committed(const HeadView& head)
{
    return ParentRef{ParentKind::COMMITTED, head.Header().height, head.HeaderHash()};
}

ParentRef ParentRef::Uncommitted(const Candidate& candidate)
{
    return ParentRef{ParentKind::CANDIDATE, candidate.Header().height, candidate.Header().Hash()};
}

Candidate::Candidate(SlotHeaderV2 header, std::vector<OutcomeV2> outcomes, std::vector<uint256> outcome_leaves,
                     std::vector<uint256> action_leaves, SlotBody body, std::shared_ptr<const CoreState> next)
    : m_header{std::move(header)},
      m_outcomes{std::move(outcomes)},
      m_outcome_leaves{std::move(outcome_leaves)},
      m_action_leaves{std::move(action_leaves)},
      m_body{std::move(body)},
      m_next{std::move(next)}
{
}

Result<Candidate> BuildCandidate(const engine::Engine& engine, const HeadView& parent, const SlotInputs& inputs)
{
    const SlotHeaderV2& head{parent.Header()};

    // 1. The authority the slot is certified under.
    if (inputs.authority.epoch < head.authority_epoch) return Invalid(SLOT_EPOCH_REGRESSED);
    if (inputs.authority.seat_set_root != parent.State().seat_set_root) return Invalid(SLOT_SEAT_SET_MISMATCH);

    // 2. The validity predicate and the canonical order.
    engine::SlotInputs engine_inputs;
    engine_inputs.domain = head.domain;
    engine_inputs.profile_hash = inputs.authority.profile_hash;
    engine_inputs.anchor = inputs.anchor;
    engine_inputs.facts_from = inputs.facts_from;
    engine_inputs.facts_to = inputs.facts_to;
    engine_inputs.body = inputs.body;
    const engine::HeadView engine_parent{parent.EngineView()};
    const Result<engine::ValidatedSlot> slot{engine::ValidateSlotInputs(engine_parent, engine_inputs, engine.Profile())};
    if (!slot.has_value()) return slot.error();

    // 3. Execution. The engine copies the whole parent state into the result.
    // TODO(rebirth:M2-LED-10) copy-on-write candidate state: same roots, work O(changed leaves).
    Result<engine::SlotResult> executed{engine.ApplySlot(engine_parent, *slot)};
    if (!executed.has_value()) return executed.error();
    engine::SlotResult result{std::move(executed).value()};

    // 4. The canonical body (O-50) and the leaves.
    SlotBody body;
    body.facts = inputs.body.facts;
    body.originals.reserve(slot->Order().size());
    for (const size_t index : slot->Order()) body.originals.push_back(inputs.body.originals[index]);
    std::vector<uint256> action_leaves;
    action_leaves.reserve(body.originals.size());
    for (const auto& original : body.originals) action_leaves.push_back(ActionLeaf(original));
    std::vector<uint256> outcome_leaves;
    outcome_leaves.reserve(result.outcomes.size());
    for (const OutcomeV2& outcome : result.outcomes) outcome_leaves.push_back(outcome.LeafHash());
    if (MerkleRoot(outcome_leaves) != result.outcomes_root) return Error{ErrorCode::INVARIANT, CANDIDATE_OUTCOMES_ROOT};
    if (result.outcomes.size() > std::numeric_limits<uint32_t>::max()) return Error{ErrorCode::INVARIANT, CANDIDATE_OUTCOME_COUNT};

    // 5. The header. ValidateSlotInputs bounds the originals by
    // max_actions_per_slot (a uint32_t) and the height by its successor check.
    SlotHeaderV2 header;
    header.version = SLOT_HEADER_VERSION;
    header.domain = head.domain;
    header.profile_hash = inputs.authority.profile_hash;
    header.authority_epoch = inputs.authority.epoch;
    header.height = slot->Height();
    header.parent_hash = parent.HeaderHash();
    // Version 2 commits to the certified value, independent of which valid
    // quorum subset supplied the proof. Legacy version 0 keeps its exact bytes.
    header.parent_cert_hash = (head.commitment_version == COMMITMENT_VERSION_WITHDRAWALS || head.commitment_version == COMMITMENT_VERSION_VIEW_BOUND) && !parent.CertHash().IsNull()
                                  ? parent.HeaderHash() : parent.CertHash();
    header.anchor = inputs.anchor;
    header.facts_from = inputs.facts_from;
    header.facts_to = inputs.facts_to;
    header.seat_set_root = inputs.authority.seat_set_root;
    header.facts_root = body.FactsRoot();
    header.actions_root = MerkleRoot(action_leaves);
    header.outcomes_root = result.outcomes_root;
    header.state_root = result.roots.Root();
    header.action_count = static_cast<uint32_t>(body.originals.size());
    header.outcome_count = static_cast<uint32_t>(result.outcomes.size());
    header.commitment_version = ExpectedCommitmentVersion(parent.State().engine.ledger.Profile());

    auto next{std::make_shared<const CoreState>(CoreState{std::move(result.state), parent.State().seat_set_root})};
    return Ok(Candidate{std::move(header), std::move(result.outcomes), std::move(outcome_leaves), std::move(action_leaves),
                        std::move(body), std::move(next)});
}

Result<Candidate> ReexecuteOn(const engine::Engine& engine, const HeadView& parent, const SlotHeaderV2& header,
                              const SlotBody& body)
{
    if (header.height == 0) return Invalid(REEXECUTE_GENESIS);
    if (header.height - 1 != parent.Header().height || header.parent_hash != parent.HeaderHash()) {
        return Error{ErrorCode::STALE_PARENT, PARENT_NOT_HEAD};
    }
    SlotInputs inputs;
    inputs.authority = header.Authority();
    inputs.anchor = header.anchor;
    inputs.facts_from = header.facts_from;
    inputs.facts_to = header.facts_to;
    inputs.body = body;
    Result<Candidate> candidate{BuildCandidate(engine, parent, inputs)};
    if (!candidate.has_value()) return candidate.error();
    if (!(candidate->Header() == header)) return Error{ErrorCode::VERIFY_FAILED, REEXECUTE_HEADER_MISMATCH};
    if (!(candidate->Body() == body)) return Error{ErrorCode::VERIFY_FAILED, REEXECUTE_BODY_MISMATCH};
    return candidate;
}

Result<HeadView> GenesisHead(const ProfileParams& profile, std::span<const MarketDescriptor> markets, const uint256& domain,
                             const AuthorityVersion& authority, const Anchor& anchor, const FactCursor& facts_start)
{
    if (authority.profile_hash != ProfileHash(profile)) return Invalid(GENESIS_PROFILE_MISMATCH);
    Result<CoreState> state{CoreState::Genesis(profile, markets, authority.seat_set_root)};
    if (!state.has_value()) return state.error();
    const Result<StateRootsV0> roots{state->engine.Commit()};
    if (!roots.has_value()) return roots.error();
    const Result<SlotHeaderV2> header{GenesisHeader(domain, authority, anchor, facts_start, roots->Root(), roots->version)};
    if (!header.has_value()) return header.error();
    return HeadView::Make(*header, uint256{}, std::make_shared<const CoreState>(std::move(state).value()));
}

} // namespace flowmesh2::state
