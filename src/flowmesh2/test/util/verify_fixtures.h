// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_TEST_UTIL_VERIFY_FIXTURES_H
#define B3_FLOWMESH2_TEST_UTIL_VERIFY_FIXTURES_H

#include <crypto/bls.h>
#include <flowmesh2/core/account_leaf.h>
#include <flowmesh2/core/action.h>
#include <flowmesh2/core/certificate.h>
#include <flowmesh2/core/merkle.h>
#include <flowmesh2/core/outcome.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/seat_set.h>
#include <flowmesh2/core/slot.h>
#include <flowmesh2/core/state_root.h>
#include <flowmesh2/state/candidate.h>
#include <flowmesh2/test/util/agreement_fixtures.h>
#include <flowmesh2/test/util/keys.h>
#include <flowmesh2/test/util/ledger_fixtures.h>
#include <flowmesh2/test/util/slot_fixtures.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/** TEST fixtures of the b3verify and read suites (slice R11). Synthetic
 * values only: BLS seats of test/util/agreement_fixtures.h (IKM
 * SHA256("fm2/test/seat/<name>")), the TEST network and signer keys of
 * test/util/slot_fixtures.h.
 *
 * - SignCertificate: a real BLS certificate of a header (tests may sign;
 *   guard rule 7 allows bls::SecretKey and .Sign( in test/).
 * - SyntheticState / SyntheticHeader: a certified-looking header that
 *   commits to a chosen set of v0 account leaves, for verifier tests that
 *   need a state the engine would take many slots to reach (a full nonce
 *   window, for example).
 * - SkeletonIds / RunSkeleton: ARCHITECTURE.md 14.2 slots 1-5 through any
 *   runner that commits one slot and reports its head and outcomes; w (the
 *   action the simulator withholds) is signed but never proposed.
 */
namespace flowmesh2::test {

/** A certificate of `header` by the seat set of `names` (k = names.size()):
 * the bitmap marks `signers`; the aggregate is the BLS aggregate of the
 * CommitDigest signatures of `aggregate_of` (default: `signers`), so a test
 * can make a bitmap and an aggregate disagree.
 */
inline SlotCertificate SignCertificate(const SlotHeaderV2& header, const std::vector<std::string>& names,
                                       const std::vector<uint32_t>& signers,
                                       const std::optional<std::vector<uint32_t>>& aggregate_of = std::nullopt)
{
    SlotCertificate cert;
    cert.header_hash = header.Hash();
    Result<std::vector<unsigned char>> bitmap{SlotCertificate::MakeBitmap(names.size(), signers)};
    RequireOk(bitmap, "MakeBitmap");
    cert.bitmap = std::move(bitmap).value();
    const uint256 digest{header.CommitDigest()};
    std::vector<bls::Signature> signatures;
    for (const uint32_t index : aggregate_of.value_or(signers)) {
        BOOST_REQUIRE(index < names.size());
        signatures.push_back(BlsKeyFromLabel(SeatLabel(names[index])).Sign(std::span<const unsigned char>{digest.begin(), digest.end()}));
    }
    const std::optional<bls::Signature> aggregate{bls::AggregateSignatures(signatures)};
    BOOST_REQUIRE(aggregate.has_value());
    cert.aggregate = aggregate->Compressed();
    return cert;
}

//! v0 roots whose accounts component holds exactly `leaves` (sorted by account here); the other components are labels.
struct SyntheticState {
    std::vector<AccountLeafV0> leaves;
    std::vector<uint256> values;
    StateRootsV0 roots;

    explicit SyntheticState(std::vector<AccountLeafV0> leaves_in) : leaves{std::move(leaves_in)}
    {
        std::sort(leaves.begin(), leaves.end(), [](const AccountLeafV0& x, const AccountLeafV0& y) { return x.account < y.account; });
        for (const AccountLeafV0& leaf : leaves) values.push_back(leaf.LeafValue());
        roots.accounts = MerkleRoot(values);
        roots.orders = LabelHash("fm2/test/synthetic/orders");
        roots.markets = LabelHash("fm2/test/synthetic/markets");
        roots.nullifiers = LabelHash("fm2/test/synthetic/nullifiers");
        roots.totals = LabelHash("fm2/test/synthetic/totals");
    }

    //! The leaf of `account` and its proof; fails the test if absent.
    std::pair<AccountLeafV0, MerkleProof> Witness(const AccountId& account) const
    {
        for (size_t i{0}; i < leaves.size(); ++i) {
            if (leaves[i].account != account) continue;
            Result<MerkleProof> proof{MerkleProve(values, i)};
            RequireOk(proof, "MerkleProve");
            return {leaves[i], std::move(proof).value()};
        }
        BOOST_FAIL("no synthetic leaf for the account");
        return {};
    }
};

//! A header at `height` that commits to `roots` under `seats` and the regtest profile; the other fields are labels.
inline SlotHeaderV2 SyntheticHeader(const SeatSet& seats, uint64_t height, const StateRootsV0& roots)
{
    SlotHeaderV2 header;
    header.domain = TestDomain();
    header.profile_hash = ProfileHash(RegtestProfile());
    header.authority_epoch = 0;
    header.height = height;
    header.parent_hash = LabelHash("fm2/test/synthetic/parent/" + std::to_string(height));
    header.parent_cert_hash = LabelHash("fm2/test/synthetic/parent_cert/" + std::to_string(height));
    header.anchor = TestAnchor(130);
    header.facts_from = FactCursor{130, 0};
    header.facts_to = FactCursor{130, 0};
    header.seat_set_root = seats.Root();
    header.facts_root = MerkleRoot({});
    header.actions_root = MerkleRoot({});
    header.outcomes_root = MerkleRoot({});
    header.state_root = roots.Root();
    return header;
}

//! An account leaf of `name` holding `rusd` available rUSD, with `used` nonces.
inline AccountLeafV0 LeafOf(std::string_view name, Atoms rusd, std::vector<uint64_t> used)
{
    AccountLeafV0 leaf;
    leaf.account = KeyAccount(name);
    if (rusd != 0) leaf.balances.push_back(AssetBalance{TestAsset("rUSD"), rusd, 0});
    leaf.used_nonces = std::move(used);
    return leaf;
}

//! What one committed slot produced: its head and its outcomes in emission order.
struct CommittedSlot {
    std::shared_ptr<const state::HeadView> head;
    std::vector<OutcomeV2> outcomes;
};

//! The actions of ARCHITECTURE.md 14.2 and their ids.
struct SkeletonIds {
    OrderId a; //!< Bob SELL M1 GTC 5,000 at 250, n=1 (slot 2)
    OrderId b; //!< Alice BUY M2 GTC 100,000 at 600, n=1 (slot 2)
    OrderId c; //!< Alice BUY M1 IOC 2,000 at 260, n=2 (slot 3)
    OrderId d; //!< Alice BUY M2 GTC 100,000 at 600, n=3 (slot 3, REJ_INSUFFICIENT_AVAILABLE)
    ActionId e; //!< Bob TRANSFER 100,000 rUSD to Alice, n=2 (slot 4)
    ActionId f; //!< Alice CANCEL b, n=4 (slot 4)
    std::vector<unsigned char> c_bytes;
    std::vector<unsigned char> d_bytes;
    std::vector<unsigned char> w_bytes; //!< Alice BUY M1 GTC 1 at 1, n=5, ref_slot 3, max_delay 1: never proposed
};

/** Slots 1..`slots` (at most 5) of the walking skeleton. `run(facts,
 * originals)` commits one slot after the current head and returns it;
 * `actions` must follow the head (its header is the head's when an action
 * is signed, so ref_slot is the head height). w is signed at head 3.
 */
template <class Run>
std::vector<CommittedSlot> RunSkeleton(SlotFixture& actions, Run&& run, SkeletonIds& ids, size_t slots = 5)
{
    std::vector<CommittedSlot> out;
    const AssetId rusd{TestAsset("rUSD")};
    SlotFixture& a{actions};
    if (slots >= 1) {
        const auto d1{a.DepositBytes(KeyAccount("alice"), rusd, 1'000'000'000, "T1")};
        const auto d2{a.DepositBytes(KeyAccount("bob"), AssetId::Native(), 10'000'000'000, "T2")};
        const auto d3{a.DepositBytes(KeyAccount("bob"), TestAsset("TOK"), 500'000, "T3")};
        out.push_back(run({d1, d2, d3}, {}));
    }
    if (slots >= 2) {
        const auto order_a{a.Place("bob", 1, MarketM1(), Side::SELL, TimeInForce::GTC, 250, 5'000)};
        const auto order_b{a.Place("alice", 1, MarketM2(), Side::BUY, TimeInForce::GTC, 600, 100'000)};
        ids.a = a.IdOf(order_a);
        ids.b = a.IdOf(order_b);
        out.push_back(run({}, {order_a, order_b}));
    }
    if (slots >= 3) {
        const auto c{a.Place("alice", 2, MarketM1(), Side::BUY, TimeInForce::IOC, 260, 2'000)};
        const auto d{a.Place("alice", 3, MarketM2(), Side::BUY, TimeInForce::GTC, 600, 100'000)};
        ids.c = a.IdOf(c);
        ids.d = a.IdOf(d);
        ids.c_bytes = c;
        ids.d_bytes = d;
        out.push_back(run({}, {c, d}));
        ids.w_bytes = a.Sign(a.Unsigned("alice", 5, ActionKind::PLACE,
                                        EncodePayload(PlacePayload{MarketM1().Id(), Side::BUY, TimeInForce::GTC, 1, 1}).value(),
                                        /*ref_slot=*/3, /*max_delay=*/1),
                             "alice");
    }
    if (slots >= 4) {
        const auto e{a.Transfer("bob", 2, KeyAccount("alice"), rusd, 100'000)};
        const auto cancel_b{a.Cancel("alice", 4, ids.b)};
        ids.e = ActionId{a.IdOf(e).id};
        ids.f = ActionId{a.IdOf(cancel_b).id};
        out.push_back(run({}, {e, cancel_b}));
    }
    if (slots >= 5) out.push_back(run({}, {}));
    return out;
}

//! The outcome of `subject` among `outcomes` and its index; fails the test if absent.
inline std::pair<uint32_t, OutcomeV2> FindOutcome(const std::vector<OutcomeV2>& outcomes, const uint256& subject)
{
    for (size_t i{0}; i < outcomes.size(); ++i) {
        if (outcomes[i].subject == subject) return {static_cast<uint32_t>(i), outcomes[i]};
    }
    BOOST_FAIL("no outcome for the subject");
    return {};
}

//! The decoded ActionV2 of exact original bytes.
inline ActionV2 DecodeAction(const std::vector<unsigned char>& bytes)
{
    Result<ActionV2> action{ActionV2::Decode(bytes, RegtestProfile())};
    RequireOk(action, "ActionV2::Decode");
    return std::move(action).value();
}

} // namespace flowmesh2::test

#endif // B3_FLOWMESH2_TEST_UTIL_VERIFY_FIXTURES_H
