// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_TEST_UTIL_SLOT_FIXTURES_H
#define B3_FLOWMESH2_TEST_UTIL_SLOT_FIXTURES_H

#include <flowmesh2/core/action.h>
#include <flowmesh2/core/authority.h>
#include <flowmesh2/core/body.h>
#include <flowmesh2/core/facts.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/market.h>
#include <flowmesh2/core/outcome.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/slot.h>
#include <flowmesh2/engine/engine.h>
#include <flowmesh2/ledger/ledger.h>
#include <flowmesh2/test/util/keys.h>
#include <flowmesh2/test/util/ledger_fixtures.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/** TEST fixtures of the slot engine tests (slice R7): a genesis head on a
 * synthetic TEST network, signed actions from label keys, deposit fact
 * bytes and slot inputs. Synthetic values only (ARCHITECTURE.md 14.1): the
 * domain is DomainHash of a TEST magic and SHA256("fm2/test/domain"), keys
 * are SHA256("fm2/test/key/<name>"), anchors and outpoints are labels.
 */
namespace flowmesh2::test {

//! The synthetic TEST network of the engine tests (never a real network's values).
inline NetworkId TestNetwork()
{
    NetworkId network;
    network.chain = ChainType::REGTEST;
    network.magic = {0xf2, 0x7e, 0x57, 0x01};
    network.modern_chain_domain = LabelHash("fm2/test/domain");
    return network;
}

inline uint256 TestDomain() { return DomainHash(TestNetwork(), FM_VERSION); }

//! The key label of a test signer.
inline std::string SignerLabel(std::string_view name) { return "fm2/test/key/" + std::string{name}; }
//! The AccountId of a test signer (AccountId::FromKey of its x-only key).
inline AccountId KeyAccount(std::string_view name) { return AccountId::FromKey(XOnlyFromLabel(SignerLabel(name))); }

inline Anchor TestAnchor(uint32_t height) { return Anchor{height, LabelHash("fm2/test/anchor/" + std::to_string(height))}; }

//! A head (header + engine state) and helpers to build the next slot's inputs.
struct SlotFixture {
    ProfileParams profile;
    std::vector<MarketDescriptor> markets;
    uint256 domain;
    uint256 profile_hash;
    SlotHeaderV2 header;
    engine::EngineState state;
    uint32_t anchor_height{130};
    uint32_t next_fact_index{0}; //!< index of the next deposit cursor at anchor_height
    uint64_t aux{1};             //!< BIP340 aux randomness counter (any value is valid)

    explicit SlotFixture(const ProfileParams& profile_in = RegtestProfile(),
                         std::vector<MarketDescriptor> markets_in = {MarketM1(), MarketM2()})
        : profile{profile_in},
          markets{std::move(markets_in)},
          domain{TestDomain()},
          profile_hash{ProfileHash(profile_in)},
          state{Genesis(profile_in, markets)}
    {
        const Result<StateRootsV0> roots{state.Commit()};
        RequireOk(roots, "genesis commit");
        const AuthorityVersion authority{0, LabelHash("fm2/test/seatset"), profile_hash};
        const Result<SlotHeaderV2> genesis{GenesisHeader(domain, authority, TestAnchor(anchor_height - 1),
                                                         FactCursor{anchor_height, 0}, roots->Root(), roots->version)};
        RequireOk(genesis, "genesis header");
        header = *genesis;
    }

    static engine::EngineState Genesis(const ProfileParams& profile, const std::vector<MarketDescriptor>& markets)
    {
        Result<engine::EngineState> state{engine::EngineState::Genesis(profile, markets)};
        RequireOk(state, "EngineState::Genesis");
        return std::move(state).value();
    }

    engine::HeadView Head() const { return engine::HeadView{header, state}; }

    // ---- Actions ----------------------------------------------------------------

    //! An unsigned action of `name`; ref_slot defaults to the head height (the client's view).
    ActionV2 Unsigned(std::string_view name, uint64_t nonce, ActionKind kind, std::vector<unsigned char> payload,
                      std::optional<uint64_t> ref_slot = std::nullopt, uint16_t max_delay = 16) const
    {
        ActionV2 a;
        a.domain = domain;
        a.signer = XOnlyFromLabel(SignerLabel(name));
        a.nonce = nonce;
        a.ref_slot = ref_slot.value_or(header.height);
        a.max_delay = max_delay;
        a.kind = kind;
        a.payload = std::move(payload);
        return a;
    }
    //! `a` signed by `name`, as exact original bytes.
    std::vector<unsigned char> Sign(ActionV2 a, std::string_view name)
    {
        a.sig = SignDigest(KeyFromLabel(SignerLabel(name)), a.Id().id, uint256{static_cast<uint8_t>(aux++ & 0xff)});
        return a.Encode();
    }
    std::vector<unsigned char> Place(std::string_view name, uint64_t nonce, const MarketDescriptor& market, Side side,
                                     TimeInForce tif, uint64_t price, uint64_t lots)
    {
        return Sign(Unsigned(name, nonce, ActionKind::PLACE,
                             EncodePayload(PlacePayload{market.Id(), side, tif, price, lots}).value()),
                    name);
    }
    std::vector<unsigned char> Cancel(std::string_view name, uint64_t nonce, const OrderId& order)
    {
        return Sign(Unsigned(name, nonce, ActionKind::CANCEL, EncodePayload(CancelPayload{order}).value()), name);
    }
    std::vector<unsigned char> Transfer(std::string_view name, uint64_t nonce, const AccountId& to, const AssetId& asset,
                                        Atoms amount)
    {
        return Sign(Unsigned(name, nonce, ActionKind::TRANSFER, EncodePayload(TransferPayload{to, asset, amount}).value()),
                    name);
    }
    //! The ActionId (= OrderId of a PLACE) of exact original bytes.
    OrderId IdOf(const std::vector<unsigned char>& original) const
    {
        const Result<ActionV2> action{ActionV2::Decode(original, profile)};
        RequireOk(action, "IdOf decode");
        return OrderId::FromPlace(action->Id());
    }

    // ---- Facts and inputs -------------------------------------------------------

    //! Canonical bytes of a deposit at the next cursor (anchor_height, next_fact_index).
    std::vector<unsigned char> DepositBytes(const AccountId& account, const AssetId& asset, Atoms amount,
                                            std::string_view outpoint_label)
    {
        DepositFact fact;
        fact.cursor = FactCursor{anchor_height, next_fact_index++};
        fact.outpoint = TestOutpoint(outpoint_label, 0);
        fact.account = account;
        fact.asset = asset;
        fact.amount = amount;
        const Result<std::vector<unsigned char>> bytes{EncodeFact(Fact{fact})};
        RequireOk(bytes, "EncodeFact");
        return *bytes;
    }

    //! Inputs that follow the head: facts_from = parent facts_to, facts_to = the next deposit cursor.
    engine::SlotInputs Inputs(std::vector<std::vector<unsigned char>> facts, std::vector<std::vector<unsigned char>> originals) const
    {
        engine::SlotInputs inputs;
        inputs.domain = domain;
        inputs.profile_hash = profile_hash;
        inputs.anchor = TestAnchor(anchor_height);
        inputs.facts_from = header.facts_to;
        inputs.facts_to = FactCursor{anchor_height, next_fact_index};
        inputs.body.facts = std::move(facts);
        inputs.body.originals = std::move(originals);
        return inputs;
    }

    Result<engine::ValidatedSlot> Validate(const engine::SlotInputs& inputs) const
    {
        return engine::ValidateSlotInputs(Head(), inputs, profile);
    }

    //! Validate and execute `inputs` on the head without advancing it.
    Result<engine::SlotResult> TryApply(const engine::SlotInputs& inputs) const
    {
        const Result<engine::ValidatedSlot> slot{Validate(inputs)};
        if (!slot.has_value()) return slot.error();
        const Result<engine::Engine> engine{engine::Engine::Create(profile)};
        if (!engine.has_value()) return engine.error();
        return engine->ApplySlot(Head(), *slot);
    }

    //! Validate, execute and require success, then advance the head.
    engine::SlotResult Apply(const engine::SlotInputs& inputs)
    {
        Result<engine::SlotResult> result{TryApply(inputs)};
        RequireOk(result, "validate and apply the slot");
        Advance(inputs, *result);
        return std::move(result).value();
    }

    /** The next head: a header assembled as the state owner (R9) will
     * (parent link, inputs, roots, counts; epoch and seat set unchanged; no
     * parent certificate before R10) and the result's state.
     */
    void Advance(const engine::SlotInputs& inputs, const engine::SlotResult& result)
    {
        SlotHeaderV2 next;
        next.domain = header.domain;
        next.profile_hash = header.profile_hash;
        next.authority_epoch = header.authority_epoch;
        next.height = header.height + 1;
        next.parent_hash = header.Hash();
        next.parent_cert_hash = uint256{};
        next.anchor = inputs.anchor;
        next.facts_from = inputs.facts_from;
        next.facts_to = inputs.facts_to;
        next.seat_set_root = header.seat_set_root;
        next.facts_root = inputs.body.FactsRoot();
        next.actions_root = inputs.body.ActionsRoot();
        next.outcomes_root = result.outcomes_root;
        next.state_root = result.roots.Root();
        next.commitment_version = result.roots.version;
        next.action_count = static_cast<uint32_t>(inputs.body.originals.size());
        next.outcome_count = static_cast<uint32_t>(result.outcomes.size());
        header = next;
        state = result.state;
    }

    // ---- Reads ------------------------------------------------------------------

    ledger::Balance BalanceOf(std::string_view name, const AssetId& asset) const
    {
        return state.ledger.BalanceOf(KeyAccount(name), asset);
    }
    std::vector<uint64_t> UsedNonces(std::string_view name) const
    {
        const ledger::AccountState* account{state.ledger.Find(KeyAccount(name))};
        BOOST_REQUIRE(account != nullptr);
        return {account->nonces.Used().begin(), account->nonces.Used().end()};
    }
};

//! The outcome whose subject is `subject` (an ActionId, FactId or AssetId); fails if absent or repeated.
inline const OutcomeV2& OutcomeOf(const engine::SlotResult& result, const uint256& subject)
{
    const OutcomeV2* found{nullptr};
    for (const OutcomeV2& outcome : result.outcomes) {
        if (outcome.subject == subject) {
            BOOST_REQUIRE_MESSAGE(found == nullptr, "two outcomes with one subject");
            found = &outcome;
        }
    }
    BOOST_REQUIRE_MESSAGE(found != nullptr, "no outcome for the subject");
    return *found;
}

//! Requires an INVALID_INPUT refusal whose detail is `detail`.
template <class T>
void RequireRefusal(const Result<T>& result, std::string_view detail, std::string_view what)
{
    if (result.has_value()) BOOST_FAIL(std::string{what} + " unexpectedly validated");
    BOOST_CHECK_MESSAGE(result.error().code == ErrorCode::INVALID_INPUT && result.error().detail == detail,
                        std::string{what} + ": got " + std::string{ToString(result.error().code)} + " '" +
                            std::string{result.error().detail} + "', want INVALID_INPUT '" + std::string{detail} + "'");
}

} // namespace flowmesh2::test

#endif // B3_FLOWMESH2_TEST_UTIL_SLOT_FIXTURES_H
