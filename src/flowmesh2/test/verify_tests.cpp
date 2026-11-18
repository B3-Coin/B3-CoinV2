// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** b3verify (slice R11): every check passes on real or synthetic certified
 * data and fails, with its own static detail, on each defect the slice
 * names: a wrong seat set, a tampered header, a bitmap below the quorum, a
 * bit at or above k, an aggregate of another subset, a tampered outcome
 * proof, a wrong fee or delta, and the three VerifyExpired negatives.
 * Real data is the walking skeleton (ARCHITECTURE.md 14.2) through the
 * state owner; its certificates are test certificates, because these
 * checks do not read them. Certificates that are checked are real BLS
 * aggregates (test/util/verify_fixtures.h).
 */

#include <boost/test/unit_test.hpp>

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
#include <flowmesh2/ledger/state_commitment.h>
#include <flowmesh2/state/candidate.h>
#include <flowmesh2/test/util/state_fixtures.h>
#include <flowmesh2/test/util/verify_fixtures.h>
#include <flowmesh2/verify/b3verify.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::test;

namespace {

//! Requires VERIFY_FAILED with exactly `detail`.
void RequireVerifyFailed(const Result<void>& result, std::string_view detail, std::string_view what)
{
    RequireFailure(result, ErrorCode::VERIFY_FAILED, detail, what);
}

const std::vector<std::string> FOUR{"v0", "v1", "v2", "v3"};

//! A header committed to by the four test seats (k = 4, quorum 3).
struct FourSeatHeader {
    SeatSet seats{TestSeats(FOUR)};
    SyntheticState state{{LeafOf("alice", 1'000, {1, 2})}};
    SlotHeaderV2 header{SyntheticHeader(seats, 7, state.roots)};
};

//! Walking-skeleton slots 1-3 through the state owner (test certificates), with their outcomes.
struct SkeletonToSlot3 {
    OwnerFixture f;
    SkeletonIds ids;
    std::vector<CommittedSlot> slots;

    SkeletonToSlot3()
    {
        slots = RunSkeleton(
            f.actions,
            [this](std::vector<std::vector<unsigned char>> facts, std::vector<std::vector<unsigned char>> originals) {
                state::Candidate candidate{f.Preview(f.Inputs(std::move(facts), std::move(originals)))};
                std::vector<OutcomeV2> outcomes{candidate.Outcomes()};
                return CommittedSlot{f.Commit(std::move(candidate)), std::move(outcomes)};
            },
            ids, 3);
    }

    const SlotHeaderV2& Header(size_t slot) const { return slots.at(slot - 1).head->Header(); }
    const std::vector<OutcomeV2>& Outcomes(size_t slot) const { return slots.at(slot - 1).outcomes; }

    //! The leaf of `name` after `slot` and its proof, with the state's roots.
    std::tuple<AccountLeafV0, MerkleProof, StateRootsV0> Leaf(size_t slot, std::string_view name) const
    {
        const state::CoreState& state{slots.at(slot - 1).head->State()};
        Result<ledger::AccountWitnessV0> witness{ledger::ProveAccountV0(state.engine.ledger, KeyAccount(name))};
        RequireOk(witness, "ProveAccountV0");
        Result<StateRootsV0> roots{state.engine.Commit()};
        RequireOk(roots, "Commit");
        return {witness->leaf, witness->proof, *roots};
    }

    //! The proof of outcome `index` of `slot`.
    MerkleProof OutcomeProof(size_t slot, uint32_t index) const
    {
        std::vector<uint256> leaves;
        for (const OutcomeV2& outcome : Outcomes(slot)) leaves.push_back(outcome.LeafHash());
        Result<MerkleProof> proof{MerkleProve(leaves, index)};
        RequireOk(proof, "MerkleProve");
        return std::move(proof).value();
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(verify_tests)

BOOST_AUTO_TEST_CASE(certificate_passes_for_every_quorum_subset)
{
    const FourSeatHeader h;
    BOOST_REQUIRE_EQUAL(h.seats.Quorum(), 3U);
    // Certificates are not unique (O-4b): every quorum subset verifies.
    for (const std::vector<uint32_t>& signers : {std::vector<uint32_t>{0, 1, 2}, std::vector<uint32_t>{1, 2, 3},
                                                 std::vector<uint32_t>{0, 2, 3}, std::vector<uint32_t>{0, 1, 2, 3}}) {
        RequireOk(verify::VerifyCertificate(h.header, SignCertificate(h.header, FOUR, signers), h.seats), "quorum subset");
    }
    // The k = 1 dev seat: bitmap 0x01 is the only valid one.
    const SeatSet one{TestSeats({"s0"})};
    const SlotHeaderV2 dev{SyntheticHeader(one, 3, h.state.roots)};
    const SlotCertificate cert{SignCertificate(dev, {"s0"}, {0})};
    BOOST_CHECK(cert.bitmap == std::vector<unsigned char>{0x01});
    RequireOk(verify::VerifyCertificate(dev, cert, one), "k = 1");
}

BOOST_AUTO_TEST_CASE(certificate_fails_for_each_defect)
{
    const FourSeatHeader h;
    const SlotCertificate good{SignCertificate(h.header, FOUR, {0, 1, 2})};
    RequireOk(verify::VerifyCertificate(h.header, good, h.seats), "control");

    // A wrong seat set: the header names the trusted one, the client trusts another.
    const SeatSet other{TestSeats({"w0", "w1", "w2", "w3"})};
    RequireVerifyFailed(verify::VerifyCertificate(h.header, good, other), verify::CERT_SEAT_SET, "wrong seat set");
    // A header that names the other set, certified by the original seats' keys.
    SlotHeaderV2 renamed{h.header};
    renamed.seat_set_root = other.Root();
    RequireVerifyFailed(verify::VerifyCertificate(renamed, SignCertificate(renamed, FOUR, {0, 1, 2}), other),
                        verify::CERT_SIGNATURE, "signed by seats outside the set");

    // A tampered header: the certificate no longer names it; renaming the
    // certificate leaves an aggregate over the old CommitDigest.
    SlotHeaderV2 tampered{h.header};
    tampered.state_root = LabelHash("fm2/test/tampered/state");
    RequireVerifyFailed(verify::VerifyCertificate(tampered, good, h.seats), verify::CERT_HEADER, "tampered header");
    SlotCertificate renamed_cert{good};
    renamed_cert.header_hash = tampered.Hash();
    RequireVerifyFailed(verify::VerifyCertificate(tampered, renamed_cert, h.seats), verify::CERT_SIGNATURE,
                        "tampered header, renamed certificate");
    for (const auto& field : {&SlotHeaderV2::outcomes_root, &SlotHeaderV2::actions_root, &SlotHeaderV2::parent_hash}) {
        SlotHeaderV2 t{h.header};
        t.*field = LabelHash("fm2/test/tampered/field");
        SlotCertificate c{good};
        c.header_hash = t.Hash();
        RequireVerifyFailed(verify::VerifyCertificate(t, c, h.seats), verify::CERT_SIGNATURE, "tampered root");
    }

    // A bitmap below the quorum, with a matching aggregate.
    RequireVerifyFailed(verify::VerifyCertificate(h.header, SignCertificate(h.header, FOUR, {0, 1}), h.seats),
                        verify::CERT_QUORUM, "two of four");
    SlotCertificate empty{good};
    empty.bitmap = {0x00};
    RequireVerifyFailed(verify::VerifyCertificate(h.header, empty, h.seats), verify::CERT_QUORUM, "no signer");

    // A bit at or above k (k = 4: bits 4..7 of the one byte), and a wrong length.
    for (const unsigned char extra : {0x10, 0x20, 0x40, 0x80}) {
        SlotCertificate high{good};
        high.bitmap = {static_cast<unsigned char>(0x07 | extra)};
        RequireVerifyFailed(verify::VerifyCertificate(h.header, high, h.seats), verify::CERT_BITMAP, "bit >= k");
    }
    SlotCertificate longer{good};
    longer.bitmap.push_back(0x00);
    RequireVerifyFailed(verify::VerifyCertificate(h.header, longer, h.seats), verify::CERT_BITMAP, "two-byte bitmap");
    SlotCertificate shorter{good};
    shorter.bitmap.clear();
    RequireVerifyFailed(verify::VerifyCertificate(h.header, shorter, h.seats), verify::CERT_BITMAP, "empty bitmap");

    // An aggregate of another quorum subset than the bitmap marks.
    RequireVerifyFailed(verify::VerifyCertificate(h.header, SignCertificate(h.header, FOUR, {0, 1, 2}, std::vector<uint32_t>{0, 1, 3}), h.seats),
                        verify::CERT_SIGNATURE, "aggregate of {0,1,3} under bitmap {0,1,2}");
    RequireVerifyFailed(verify::VerifyCertificate(h.header, SignCertificate(h.header, FOUR, {0, 1, 2, 3}, std::vector<uint32_t>{0, 1, 2}), h.seats),
                        verify::CERT_SIGNATURE, "aggregate of three under bitmap of four");

    // An aggregate that is not a point.
    SlotCertificate garbage{good};
    garbage.aggregate.fill(0x00);
    BOOST_REQUIRE(!bls::Signature::Decode(garbage.aggregate).has_value());
    RequireVerifyFailed(verify::VerifyCertificate(h.header, garbage, h.seats), verify::CERT_AGGREGATE, "zero aggregate");

    // k = 1: no signer, and a bit above the seat.
    const SeatSet one{TestSeats({"s0"})};
    const SlotHeaderV2 dev{SyntheticHeader(one, 3, h.state.roots)};
    SlotCertificate dev_cert{SignCertificate(dev, {"s0"}, {0})};
    dev_cert.bitmap = {0x00};
    RequireVerifyFailed(verify::VerifyCertificate(dev, dev_cert, one), verify::CERT_QUORUM, "k = 1, bitmap 0x00");
    dev_cert.bitmap = {0x03};
    RequireVerifyFailed(verify::VerifyCertificate(dev, dev_cert, one), verify::CERT_BITMAP, "k = 1, bitmap 0x03");
}

BOOST_AUTO_TEST_CASE(outcome_proofs_pass_and_tampering_fails)
{
    const SkeletonToSlot3 s;
    for (size_t slot{1}; slot <= 3; ++slot) {
        BOOST_REQUIRE_EQUAL(s.Outcomes(slot).size(), s.Header(slot).outcome_count);
        for (uint32_t i{0}; i < s.Outcomes(slot).size(); ++i) {
            RequireOk(verify::VerifyOutcome(s.Header(slot), s.Outcomes(slot)[i], s.OutcomeProof(slot, i)), "certified outcome");
        }
    }
    const auto [index, c] = FindOutcome(s.Outcomes(3), s.ids.c.id);
    const MerkleProof proof{s.OutcomeProof(3, index)};
    BOOST_REQUIRE(!c.deltas.empty());

    OutcomeV2 wrong_delta{c};
    wrong_delta.deltas.front().available_delta += 1;
    RequireVerifyFailed(verify::VerifyOutcome(s.Header(3), wrong_delta, proof), verify::OUTCOME_PROOF, "tampered delta");
    OutcomeV2 wrong_fee{c};
    wrong_fee.fills.front().seller_fee += 1;
    RequireVerifyFailed(verify::VerifyOutcome(s.Header(3), wrong_fee, proof), verify::OUTCOME_PROOF, "tampered fee");
    OutcomeV2 wrong_code{c};
    wrong_code.code = OutcomeCode::PARTIAL_RESTING;
    RequireVerifyFailed(verify::VerifyOutcome(s.Header(3), wrong_code, proof), verify::OUTCOME_PROOF, "tampered code");

    MerkleProof bad_path{proof};
    BOOST_REQUIRE(!bad_path.path.empty());
    bad_path.path.front() = LabelHash("fm2/test/tampered/path");
    RequireVerifyFailed(verify::VerifyOutcome(s.Header(3), c, bad_path), verify::OUTCOME_PROOF, "tampered path");
    MerkleProof bad_index{proof};
    bad_index.index ^= 1;
    RequireVerifyFailed(verify::VerifyOutcome(s.Header(3), c, bad_index), verify::OUTCOME_PROOF, "other index");
    MerkleProof bad_size{proof};
    bad_size.size += 1;
    RequireVerifyFailed(verify::VerifyOutcome(s.Header(3), c, bad_size), verify::OUTCOME_PROOF_SIZE, "other size");
    // The outcome of slot 3 is not an outcome of slot 2.
    RequireVerifyFailed(verify::VerifyOutcome(s.Header(2), c, proof), verify::OUTCOME_PROOF_SIZE, "other header");
}

BOOST_AUTO_TEST_CASE(outcome_deltas_must_be_canonical)
{
    // A header that certifies one outcome whose deltas break the engine's form.
    const SeatSet one{TestSeats({"s0"})};
    const SyntheticState state{{LeafOf("alice", 1, {})}};
    const BalanceDelta low{KeyAccount("alice"), TestAsset("rUSD"), 5, 0};
    BalanceDelta high{low};
    high.asset = TestAsset("TOK");
    BalanceDelta zero{low};
    zero.available_delta = 0;
    const std::vector<std::pair<std::vector<BalanceDelta>, bool>> cases{
        {{}, true},
        {{low}, true},
        {low.asset < high.asset ? std::vector<BalanceDelta>{low, high} : std::vector<BalanceDelta>{high, low}, true},
        {low.asset < high.asset ? std::vector<BalanceDelta>{high, low} : std::vector<BalanceDelta>{low, high}, false},
        {{low, low}, false},
        {{zero}, false},
    };
    for (const auto& [deltas, canonical] : cases) {
        OutcomeV2 outcome;
        outcome.subject = LabelHash("fm2/test/outcome");
        outcome.account = KeyAccount("alice");
        outcome.code = OutcomeCode::TRANSFERRED;
        outcome.deltas = deltas;
        SlotHeaderV2 header{SyntheticHeader(one, 2, state.roots)};
        header.outcomes_root = MerkleRoot(std::vector<uint256>{outcome.LeafHash()});
        header.outcome_count = 1;
        const Result<MerkleProof> proof{MerkleProve(std::vector<uint256>{outcome.LeafHash()}, 0)};
        RequireOk(proof, "MerkleProve");
        if (canonical) {
            RequireOk(verify::VerifyOutcome(header, outcome, *proof), "canonical deltas");
        } else {
            RequireVerifyFailed(verify::VerifyOutcome(header, outcome, *proof), verify::OUTCOME_DELTAS, "non-canonical deltas");
        }
    }
}

BOOST_AUTO_TEST_CASE(account_leaf_under_the_state_root)
{
    const SkeletonToSlot3 s;
    const ProfileParams profile{RegtestProfile()};
    for (const char* name : {"alice", "bob"}) {
        const auto [leaf, proof, roots] = s.Leaf(3, name);
        RequireOk(verify::VerifyAccountLeaf(s.Header(3), leaf, roots, proof, profile), "leaf at slot 3");
    }
    const auto [leaf, proof, roots] = s.Leaf(3, "alice");
    AccountLeafV0 richer{leaf};
    richer.balances.front().available += 1;
    RequireVerifyFailed(verify::VerifyAccountLeaf(s.Header(3), richer, roots, proof, profile), verify::LEAF_PROOF, "tampered balance");
    AccountLeafV0 fewer_nonces{leaf};
    fewer_nonces.used_nonces.pop_back();
    RequireVerifyFailed(verify::VerifyAccountLeaf(s.Header(3), fewer_nonces, roots, proof, profile), verify::LEAF_PROOF, "tampered nonces");
    AccountLeafV0 zero_entry{leaf};
    zero_entry.balances.push_back(AssetBalance{AssetId{LabelHash("fm2/test/zz")}, 0, 0});
    RequireVerifyFailed(verify::VerifyAccountLeaf(s.Header(3), zero_entry, roots, proof, profile), verify::LEAF_NOT_CANONICAL,
                        "all-zero entry");
    StateRootsV0 other_roots{roots};
    other_roots.orders = LabelHash("fm2/test/tampered/orders");
    RequireVerifyFailed(verify::VerifyAccountLeaf(s.Header(3), leaf, other_roots, proof, profile), verify::LEAF_STATE_ROOTS,
                        "tampered component root");
    // Slot 2's leaf is not under slot 3's state root.
    const auto [leaf2, proof2, roots2] = s.Leaf(2, "alice");
    RequireVerifyFailed(verify::VerifyAccountLeaf(s.Header(3), leaf2, roots, proof2, profile), verify::LEAF_PROOF, "stale leaf");
    RequireVerifyFailed(verify::VerifyAccountLeaf(s.Header(3), leaf2, roots2, proof2, profile), verify::LEAF_STATE_ROOTS, "stale roots");
    // Bob's proof does not prove Alice's leaf.
    const auto [bob_leaf, bob_proof, bob_roots] = s.Leaf(3, "bob");
    RequireVerifyFailed(verify::VerifyAccountLeaf(s.Header(3), leaf, roots, bob_proof, profile), verify::LEAF_PROOF, "other proof");
    // Another profile is not the header's.
    ProfileParams other{profile};
    other.fee_ppm = 200;
    RequireVerifyFailed(verify::VerifyAccountLeaf(s.Header(3), leaf, roots, proof, other), verify::PROFILE_MISMATCH, "other profile");
    ProfileParams per_side{profile};
    per_side.fee_incidence = FeeIncidence::PER_SIDE;
    RequireStub(verify::VerifyAccountLeaf(s.Header(3), leaf, roots, proof, per_side), "M3-ENG-08");
}

BOOST_AUTO_TEST_CASE(fill_quote_and_fee_recomputed)
{
    const SkeletonToSlot3 s;
    const ProfileParams profile{RegtestProfile()};
    const auto [index, c] = FindOutcome(s.Outcomes(3), s.ids.c.id);
    BOOST_REQUIRE_EQUAL(c.fills.size(), 1U);
    const Fill& fill{c.fills.front()};
    BOOST_CHECK(fill.maker == s.ids.a);
    BOOST_CHECK(fill.taker == s.ids.c);
    BOOST_CHECK_EQUAL(fill.quote_atoms, 500'000);
    BOOST_CHECK_EQUAL(fill.seller_fee, 50);
    RequireOk(verify::VerifyFill(fill, MarketM1(), profile), "walking-skeleton fill");

    const auto fails = [&](Fill f, std::string_view detail, std::string_view what) {
        RequireVerifyFailed(verify::VerifyFill(f, MarketM1(), profile), detail, what);
    };
    Fill f{fill};
    f.seller_fee = 51;
    fails(f, verify::FILL_FEE, "fee + 1");
    f.seller_fee = 49;
    fails(f, verify::FILL_FEE, "fee - 1");
    f = fill;
    f.quote_atoms += 1;
    fails(f, verify::FILL_QUOTE, "quote + 1");
    f = fill;
    f.lots += 1;
    fails(f, verify::FILL_QUOTE, "lots + 1");
    f = fill;
    f.price_ticks -= 1;
    fails(f, verify::FILL_QUOTE, "price - 1");
    f = fill;
    f.lots = 0;
    f.quote_atoms = 0;
    fails(f, verify::FILL_LOTS, "zero lots");
    f = fill;
    f.price_ticks = std::numeric_limits<uint64_t>::max();
    fails(f, verify::FILL_QUOTE, "quote overflows");
    RequireVerifyFailed(verify::VerifyFill(fill, MarketM2(), profile), verify::FILL_MARKET, "other market");

    // The fee is an increment on the seller's cumulative quote: 21 atoms at
    // 100 ppm cost 1 atom from zero, and 0 atoms once 9,979 were matched
    // before (ceil(1.0000) - ceil(0.9979)).
    Fill small;
    small.maker = s.ids.a;
    small.taker = s.ids.c;
    small.market = MarketM1().Id();
    small.taker_side = Side::BUY;
    small.price_ticks = 7;
    small.lots = 3;
    small.quote_atoms = 21;
    small.seller_fee = 1;
    RequireOk(verify::VerifyFill(small, MarketM1(), profile), "first 21 atoms");
    small.seller_quote_before = 9'979;
    RequireVerifyFailed(verify::VerifyFill(small, MarketM1(), profile), verify::FILL_FEE, "fee ignores the cumulative quote");
    small.seller_fee = 0;
    RequireOk(verify::VerifyFill(small, MarketM1(), profile), "21 atoms after 9,979");

    ProfileParams double_fee{profile};
    double_fee.fee_ppm = 200;
    RequireVerifyFailed(verify::VerifyFill(fill, MarketM1(), double_fee), verify::FILL_FEE, "other fee rate");
    ProfileParams per_side{profile};
    per_side.fee_incidence = FeeIncidence::PER_SIDE;
    RequireStub(verify::VerifyFill(fill, MarketM1(), per_side), "M3-ENG-08");
}

BOOST_AUTO_TEST_CASE(balance_transition_recomputed)
{
    const SkeletonToSlot3 s;
    const AssetId rusd{TestAsset("rUSD")};
    const auto [alice2, p2, r2] = s.Leaf(2, "alice");
    const auto [alice3, p3, r3] = s.Leaf(3, "alice");
    // ARCHITECTURE.md 14.4 P5.
    BOOST_CHECK_EQUAL(alice2.Available(rusd), 400'000'000);
    BOOST_CHECK_EQUAL(alice3.Available(rusd), 399'500'000);
    BOOST_CHECK_EQUAL(alice2.Available(AssetId::Native()), 0);
    BOOST_CHECK_EQUAL(alice3.Available(AssetId::Native()), 2'000'000'000);
    RequireOk(verify::VerifyBalanceTransition(alice2, alice3, s.Outcomes(3)), "alice slot 2 -> 3");
    // Bob's maker side through the same outcomes: rUSD +499,950, B3 reserved -2,000,000,000.
    const auto [bob2, bp2, br2] = s.Leaf(2, "bob");
    const auto [bob3, bp3, br3] = s.Leaf(3, "bob");
    BOOST_CHECK_EQUAL(bob3.Available(rusd) - bob2.Available(rusd), 499'950);
    BOOST_CHECK_EQUAL(bob3.Reserved(AssetId::Native()) - bob2.Reserved(AssetId::Native()), -2'000'000'000);
    RequireOk(verify::VerifyBalanceTransition(bob2, bob3, s.Outcomes(3)), "bob slot 2 -> 3");
    // An unchanged account: no deltas, equal balances.
    RequireOk(verify::VerifyBalanceTransition(alice3, alice3, {}), "no change");

    // A wrong delta, in each field.
    const auto [index, c] = FindOutcome(s.Outcomes(3), s.ids.c.id);
    for (int field{0}; field < 2; ++field) {
        std::vector<OutcomeV2> wrong{s.Outcomes(3)};
        for (BalanceDelta& d : wrong[index].deltas) {
            if (d.account != KeyAccount("alice")) continue;
            (field == 0 ? d.available_delta : d.reserved_delta) += 1;
            break;
        }
        RequireVerifyFailed(verify::VerifyBalanceTransition(alice2, alice3, wrong), verify::TRANSITION_BALANCE, "wrong delta");
    }
    // A missing outcome, a missing delta and an extra one.
    RequireVerifyFailed(verify::VerifyBalanceTransition(alice2, alice3, {}), verify::TRANSITION_BALANCE, "outcome missing");
    std::vector<OutcomeV2> dropped{s.Outcomes(3)};
    std::vector<BalanceDelta>& deltas{dropped[index].deltas};
    const auto alice_delta{std::find_if(deltas.begin(), deltas.end(), [](const BalanceDelta& d) { return d.account == KeyAccount("alice"); })};
    BOOST_REQUIRE(alice_delta != deltas.end());
    deltas.erase(alice_delta);
    RequireVerifyFailed(verify::VerifyBalanceTransition(alice2, alice3, dropped), verify::TRANSITION_BALANCE, "delta missing");
    RequireOk(verify::VerifyBalanceTransition(bob2, bob3, dropped), "bob's deltas untouched");
    std::vector<OutcomeV2> doubled{s.Outcomes(3)};
    doubled.push_back(c);
    RequireVerifyFailed(verify::VerifyBalanceTransition(alice2, alice3, doubled), verify::TRANSITION_BALANCE, "outcome twice");
    // Leaves of two accounts.
    RequireVerifyFailed(verify::VerifyBalanceTransition(alice2, bob3, s.Outcomes(3)), verify::TRANSITION_ACCOUNT, "two accounts");
    // Sums beyond int64 are refused, not wrapped.
    AccountLeafV0 big{LeafOf("alice", MAX_ATOMS, {})};
    OutcomeV2 plus;
    plus.deltas = {BalanceDelta{KeyAccount("alice"), rusd, MAX_ATOMS, 0}};
    RequireVerifyFailed(verify::VerifyBalanceTransition(big, big, std::vector<OutcomeV2>{plus, plus}), verify::TRANSITION_OVERFLOW,
                        "overflow");
}

BOOST_AUTO_TEST_CASE(expired_proof_passes_and_each_negative_fails)
{
    const ProfileParams profile{RegtestProfile()};
    const SeatSet one{TestSeats({"s0"})};
    SlotFixture actions;
    const std::vector<unsigned char> payload =
        EncodePayload(PlacePayload{MarketM1().Id(), Side::BUY, TimeInForce::GTC, 1, 1}).value();
    // w of ARCHITECTURE.md 14.5 N3: ref_slot 3, max_delay 1, nonce 5; Alice used {1, 2, 3, 4}.
    const ActionV2 w{actions.Unsigned("alice", 5, ActionKind::PLACE, payload, 3, 1)};
    const SyntheticState state{{LeafOf("alice", 1'000, {1, 2, 3, 4}), LeafOf("bob", 7, {1, 2})}};
    const auto [leaf, proof] = state.Witness(KeyAccount("alice"));
    const auto at = [&](uint64_t height) {
        const SlotHeaderV2 header{SyntheticHeader(one, height, state.roots)};
        return std::pair{header, SignCertificate(header, {"s0"}, {0})};
    };
    const auto [h5, c5] = at(5);
    RequireOk(verify::VerifyExpired(w, h5, c5, one, leaf, state.roots, proof, profile), "N3: 5 > 3 + 1, nonce 5 unused");

    // Height not above ref_slot + max_delay.
    for (uint64_t height : {uint64_t{4}, uint64_t{3}, uint64_t{1}}) {
        const auto [h, c] = at(height);
        RequireVerifyFailed(verify::VerifyExpired(w, h, c, one, leaf, state.roots, proof, profile), verify::EXPIRED_NOT_YET,
                            "height <= ref + max_delay");
    }
    // ExpiredAt never overflows: ref_slot at the top of u64.
    ActionV2 top{w};
    top.ref_slot = std::numeric_limits<uint64_t>::max() - 1;
    top.max_delay = 16;
    RequireVerifyFailed(verify::VerifyExpired(top, h5, c5, one, leaf, state.roots, proof, profile), verify::EXPIRED_NOT_YET,
                        "ref_slot near UINT64_MAX");

    // The nonce is in the used window.
    for (uint64_t used : {uint64_t{1}, uint64_t{4}}) {
        ActionV2 a{w};
        a.nonce = used;
        RequireVerifyFailed(verify::VerifyExpired(a, h5, c5, one, leaf, state.roots, proof, profile), verify::EXPIRED_NONCE_USED,
                            "nonce in used");
    }

    // A full window (100 nonces 101..200): below min(used) is refused, above is acceptable.
    std::vector<uint64_t> full;
    for (uint64_t n{101}; n <= 200; ++n) full.push_back(n);
    BOOST_REQUIRE_EQUAL(full.size(), profile.nonce_window);
    const SyntheticState full_state{{LeafOf("alice", 1'000, full)}};
    const auto [full_leaf, full_proof] = full_state.Witness(KeyAccount("alice"));
    const SlotHeaderV2 hf{SyntheticHeader(one, 5, full_state.roots)};
    const SlotCertificate cf{SignCertificate(hf, {"s0"}, {0})};
    for (uint64_t below : {uint64_t{0}, uint64_t{5}, uint64_t{100}}) {
        ActionV2 a{w};
        a.nonce = below;
        RequireVerifyFailed(verify::VerifyExpired(a, hf, cf, one, full_leaf, full_state.roots, full_proof, profile),
                            verify::EXPIRED_NONCE_BELOW, "window full, nonce below min(used)");
    }
    ActionV2 above{w};
    above.nonce = 201;
    RequireOk(verify::VerifyExpired(above, hf, cf, one, full_leaf, full_state.roots, full_proof, profile), "window full, nonce above");
    // A window one short of full accepts a nonce below its minimum.
    const std::vector<uint64_t> short_window(full.begin() + 1, full.end());
    const SyntheticState short_state{{LeafOf("alice", 1'000, short_window)}};
    const auto [short_leaf, short_proof] = short_state.Witness(KeyAccount("alice"));
    const SlotHeaderV2 hs{SyntheticHeader(one, 5, short_state.roots)};
    ActionV2 low{w};
    low.nonce = 5;
    RequireOk(verify::VerifyExpired(low, hs, SignCertificate(hs, {"s0"}, {0}), one, short_leaf, short_state.roots, short_proof, profile),
              "window of 99, nonce below its minimum");

    // The other checks, each with its own detail.
    ActionV2 other_domain{w};
    other_domain.domain = LabelHash("fm2/test/other/domain");
    RequireVerifyFailed(verify::VerifyExpired(other_domain, h5, c5, one, leaf, state.roots, proof, profile), verify::EXPIRED_DOMAIN,
                        "other domain");
    const auto [bob_leaf, bob_proof] = state.Witness(KeyAccount("bob"));
    RequireVerifyFailed(verify::VerifyExpired(w, h5, c5, one, bob_leaf, state.roots, bob_proof, profile), verify::EXPIRED_ACCOUNT,
                        "other account's leaf");
    for (uint16_t delay : {uint16_t{0}, uint16_t{1'025}}) {
        ActionV2 a{w};
        a.max_delay = delay;
        RequireVerifyFailed(verify::VerifyExpired(a, h5, c5, one, leaf, state.roots, proof, profile), verify::EXPIRED_MAX_DELAY,
                            "max_delay outside 1..1,024");
    }
    SlotCertificate unsigned_cert{c5};
    unsigned_cert.bitmap = {0x00};
    RequireVerifyFailed(verify::VerifyExpired(w, h5, unsigned_cert, one, leaf, state.roots, proof, profile), verify::CERT_QUORUM,
                        "uncertified header");
    AccountLeafV0 forged{leaf};
    forged.used_nonces.pop_back();
    RequireVerifyFailed(verify::VerifyExpired(w, h5, c5, one, forged, state.roots, proof, profile), verify::LEAF_PROOF,
                        "forged leaf");
}

BOOST_AUTO_TEST_CASE(stubs_never_succeed)
{
    const SeatSet one{TestSeats({"s0"})};
    const SyntheticState state{{LeafOf("alice", 1, {})}};
    const SlotHeaderV2 header{SyntheticHeader(one, 1, state.roots)};
    const SlotCertificate cert{SignCertificate(header, {"s0"}, {0})};
    RequireStub(verify::VerifyLiabilityPath(header, state.leaves.front(), verify::LiabilityPath{}), "M5-VFY-03");
    RequireStub(verify::VerifyCheckpoint(verify::CheckpointClaim{1, header.Hash(), header.state_root, uint256{}}, header, cert, one),
                "M8-CKP-01");
    RequireStub(verify::VerifyAnchorProof(header, verify::AnchorProof{header.anchor, {}, {}}), "M7-ADP-02");
}

BOOST_AUTO_TEST_CASE(stable_q_verifier_enforces_market_and_fee_outcome_asset)
{
    const ProfileParams profile{StableQRegtestProfile(ChainType::REGTEST).value()};
    Fill fill;
    fill.market = MarketM1().Id();
    fill.price_ticks = 1'001;
    fill.lots = 2;
    fill.quote_atoms = 2'002;
    fill.seller_fee = 1;
    RequireOk(verify::VerifyFill(fill, MarketM1(), profile), "stable-Q fill");
    const MarketDescriptor native_quote{TestAsset("TOK"), AssetId::Native(), 1, 1};
    fill.market = native_quote.Id();
    RequireVerifyFailed(verify::VerifyFill(fill, native_quote, profile), verify::FILL_FEE_ASSET, "native quote fee refused");
    RequireOk(verify::VerifyFill(fill, native_quote, RegtestProfile()), "legacy quote policy unchanged");

    OutcomeV2 outcome;
    outcome.source = OutcomeSource::SLOT;
    outcome.code = OutcomeCode::FEES_DISTRIBUTED;
    outcome.subject = RegtestQuoteAsset().id;
    outcome.deltas = {{AccountId{}, RegtestQuoteAsset(), 3, 0}};
    SlotHeaderV2 header;
    header.profile_hash = ProfileHash(profile);
    header.outcome_count = 1;
    const auto check = [&](const OutcomeV2& value) {
        const std::vector<uint256> leaves{value.LeafHash()};
        header.outcomes_root = MerkleRoot(leaves);
        const auto proof{MerkleProve(leaves, 0)};
        RequireOk(proof, "fee outcome proof");
        return verify::VerifyOutcome(header, value, *proof, profile);
    };
    RequireOk(check(outcome), "profile-bound Q fee outcome");
    OutcomeV2 wrong{outcome};
    wrong.subject = AssetId::Native().id;
    RequireVerifyFailed(check(wrong), verify::FEE_OUTCOME_ASSET, "native fee subject refused even with a valid proof");
    wrong = outcome;
    wrong.deltas.front().asset = AssetId::Native();
    RequireVerifyFailed(check(wrong), verify::FEE_OUTCOME_ASSET, "native fee delta refused even with a valid proof");
    wrong = outcome;
    wrong.deltas.front().available_delta = -3;
    RequireVerifyFailed(check(wrong), verify::FEE_OUTCOME_DELTA, "negative fee credit refused");
    const std::vector<uint256> leaves{outcome.LeafHash()};
    header.outcomes_root = MerkleRoot(leaves);
    header.profile_hash = ProfileHash(RegtestProfile());
    const auto proof{MerkleProve(leaves, 0)};
    RequireOk(proof, "profile mismatch proof");
    RequireVerifyFailed(verify::VerifyOutcome(header, outcome, *proof, profile), verify::PROFILE_MISMATCH, "profile downgrade refused");
}

BOOST_AUTO_TEST_CASE(account_proof_binds_withdrawal_root_and_commitment_profile_version)
{
    const auto profile{StableQRegtestProfile(ChainType::REGTEST).value()};
    SyntheticState state{{LeafOf("alice", 1000, {})}};
    state.roots.version = COMMITMENT_VERSION_WITHDRAWALS;
    state.roots.withdrawals = LabelHash("verified-withdrawals");
    SlotHeaderV2 header;
    header.profile_hash = ProfileHash(profile);
    header.commitment_version = COMMITMENT_VERSION_WITHDRAWALS;
    header.state_root = state.roots.Root();
    const auto [leaf, proof]{state.Witness(KeyAccount("alice"))};
    RequireOk(verify::VerifyAccountLeaf(header, leaf, state.roots, proof, profile), "v2 account proof");
    auto changed{state.roots};
    changed.withdrawals = LabelHash("other-withdrawals");
    RequireVerifyFailed(verify::VerifyAccountLeaf(header, leaf, changed, proof, profile), verify::LEAF_STATE_ROOTS, "sixth root tamper");
    changed = state.roots;
    changed.version = COMMITMENT_VERSION_V0;
    RequireVerifyFailed(verify::VerifyAccountLeaf(header, leaf, changed, proof, profile), verify::LEAF_STATE_ROOTS, "root version downgrade");
    auto wrong_header{header};
    wrong_header.commitment_version = COMMITMENT_VERSION_V0;
    RequireVerifyFailed(verify::VerifyAccountLeaf(wrong_header, leaf, state.roots, proof, profile), verify::LEAF_STATE_ROOTS, "header version downgrade");
}

BOOST_AUTO_TEST_CASE(withdrawal_claim_proof_verifies_exact_terms_status_and_profile)
{
    SlotFixture fx{StableQRegtestProfile(ChainType::REGTEST).value()};
    const auto alice{KeyAccount("alice")};
    fx.Apply(fx.Inputs({fx.DepositBytes(alice, RegtestQuoteAsset(), 30'000, "proof/withdraw/fund")}, {}));
    const WithdrawPayload payload{RegtestQuoteAsset(), 5'000, std::vector<unsigned char>(20, 0x46)};
    const auto action{fx.Sign(fx.Unsigned("alice", 1, ActionKind::WITHDRAW, EncodePayload(payload).value()), "alice")};
    const auto second{fx.Sign(fx.Unsigned("alice", 2, ActionKind::WITHDRAW, EncodePayload(payload).value()), "alice")};
    fx.Apply(fx.Inputs({}, {action, second}));
    const auto id{ActionV2::Decode(action, fx.profile)->Id()};
    auto witness{ledger::ProveWithdrawal(fx.state.ledger, id)};
    RequireOk(witness, "withdrawal witness");
    auto roots{fx.state.Commit().value()};
    RequireOk(verify::VerifyWithdrawal(fx.header, witness->record, roots, witness->proof, fx.profile), "pending claim inclusion");
    BOOST_CHECK_EQUAL(witness->proof.size, 2U);
    for (int mutation = 0; mutation < 7; ++mutation) {
        auto wrong{witness->record};
        if (mutation == 0) wrong.amount += 1;
        if (mutation == 1) wrong.account = KeyAccount("bob");
        if (mutation == 2) wrong.recipient[0] ^= 1;
        if (mutation == 3) wrong.id.id = LabelHash("other-claim");
        if (mutation == 4) wrong.fee += 1;
        if (mutation == 5) wrong.asset = AssetId::Native();
        if (mutation == 6) wrong.slot = fx.header.height + 1;
        BOOST_CHECK(!verify::VerifyWithdrawal(fx.header, wrong, roots, witness->proof, fx.profile).has_value());
    }
    auto wrong_roots{roots};
    wrong_roots.withdrawals = LabelHash("other-withdrawal-root");
    RequireVerifyFailed(verify::VerifyWithdrawal(fx.header, witness->record, wrong_roots, witness->proof, fx.profile),
                        verify::LEAF_STATE_ROOTS, "sixth root bound to certified state");
    BOOST_CHECK(!ledger::ProveWithdrawal(fx.state.ledger, ActionId{LabelHash("absent")}).has_value());
    BOOST_CHECK(!verify::VerifyWithdrawal(fx.header, witness->record, roots, witness->proof, RegtestProfile()).has_value());
    PayoutConfirmedFact payout;
    payout.cursor = {fx.anchor_height, fx.next_fact_index++};
    payout.withdrawal = id;
    payout.account = alice;
    payout.asset = payload.asset;
    payout.amount = payload.amount;
    payout.recipient.fill(0x46);
    payout.payout_event = TestOutpoint("proof/withdraw/payout", 0);
    payout.domain = fx.domain;
    fx.Apply(fx.Inputs({EncodeFact(Fact{payout}).value()}, {}));
    const auto paid{ledger::ProveWithdrawal(fx.state.ledger, id).value()};
    roots = fx.state.Commit().value();
    BOOST_CHECK(paid.record.paid);
    RequireOk(verify::VerifyWithdrawal(fx.header, paid.record, roots, paid.proof, fx.profile), "paid record retained and provable");
    RequireVerifyFailed(verify::VerifyWithdrawal(fx.header, witness->record, roots, witness->proof, fx.profile),
                        verify::WITHDRAWAL_PROOF, "old pending witness cannot prove current paid status");
}


BOOST_AUTO_TEST_CASE(view_bound_certificate_refuses_relabelled_and_mixed_view_shares)
{
    FourSeatHeader fixture;
    auto header{fixture.header};
    header.commitment_version = COMMITMENT_VERSION_VIEW_BOUND;
    header.profile_hash = ProfileHash(StableQViewRegtestProfile(ChainType::REGTEST).value());
    const auto good{SignCertificate(header, FOUR, {0, 1, 2}, std::nullopt, 7)};
    RequireOk(verify::VerifyCertificate(header, good, fixture.seats), "actual same-view q3");
    auto changed{good}; changed.view = 8;
    RequireVerifyFailed(verify::VerifyCertificate(header, changed, fixture.seats), verify::CERT_SIGNATURE, "relabelled genuine COMMIT shares");
    changed = good; changed.view.reset();
    RequireVerifyFailed(verify::VerifyCertificate(header, changed, fixture.seats), verify::CERT_VIEW, "stripped voting view");
    std::vector<bls::Signature> shares;
    for (uint32_t seat : {0U, 1U, 2U}) {
        const auto digest{VersionedCommitDigest(header, seat == 1 ? 8 : 7)};
        shares.push_back(BlsKeyFromLabel(SeatLabel(FOUR[seat])).Sign(std::span<const unsigned char>{digest.begin(), digest.end()}));
    }
    changed = good; changed.aggregate = bls::AggregateSignatures(shares)->Compressed();
    RequireVerifyFailed(verify::VerifyCertificate(header, changed, fixture.seats), verify::CERT_SIGNATURE, "partial COMMIT shares from separate views");
    auto archived{fixture.header}; archived.commitment_version = COMMITMENT_VERSION_WITHDRAWALS;
    const auto old_certificate{SignCertificate(archived, FOUR, {0, 1, 2})};
    RequireOk(verify::VerifyCertificate(archived, old_certificate, fixture.seats), "archived view-free certificate remains valid");
    changed = old_certificate; changed.view = 0;
    RequireVerifyFailed(verify::VerifyCertificate(archived, changed, fixture.seats), verify::CERT_VIEW, "new metadata cannot reinterpret an old signature");
}

BOOST_AUTO_TEST_CASE(view_bound_account_and_withdrawal_proofs_require_profile_version_three)
{
    const auto profile{StableQViewRegtestProfile(ChainType::REGTEST).value()};
    SyntheticState state{{LeafOf("alice", 1000, {})}};
    state.roots.version = COMMITMENT_VERSION_VIEW_BOUND;
    WithdrawalRecord record;
    record.id = ActionId{LabelHash("view-profile/withdrawal")}; record.account = KeyAccount("alice");
    record.asset = RegtestQuoteAsset(); record.amount = 5000; record.fee = profile.withdrawal_fee_atoms;
    record.recipient.fill(0x42); record.slot = 3;
    const std::vector<uint256> leaves{record.LeafValue()};
    state.roots.withdrawals = MerkleRoot(leaves);
    SlotHeaderV2 header; header.height = 4; header.profile_hash = ProfileHash(profile);
    header.commitment_version = COMMITMENT_VERSION_VIEW_BOUND; header.state_root = state.roots.Root();
    const auto [account, account_proof]{state.Witness(KeyAccount("alice"))};
    RequireOk(verify::VerifyAccountLeaf(header, account, state.roots, account_proof, profile), "v3 account proof");
    const auto proof{MerkleProve(leaves, 0).value()};
    RequireOk(verify::VerifyWithdrawal(header, record, state.roots, proof, profile), "v3 committed obligation");
    auto old_roots{state.roots}; old_roots.version = COMMITMENT_VERSION_WITHDRAWALS;
    RequireVerifyFailed(verify::VerifyAccountLeaf(header, account, old_roots, account_proof, profile), verify::LEAF_STATE_ROOTS, "old root prefix refused");
    RequireVerifyFailed(verify::VerifyWithdrawal(header, record, old_roots, proof, profile), verify::WITHDRAWAL_POLICY, "old withdrawal root version refused");
    RequireVerifyFailed(verify::VerifyWithdrawal(header, record, state.roots, proof, StableQRegtestProfile(ChainType::REGTEST).value()),
                        verify::PROFILE_MISMATCH, "archived profile cannot reinterpret current proof");
}

BOOST_AUTO_TEST_SUITE_END()
