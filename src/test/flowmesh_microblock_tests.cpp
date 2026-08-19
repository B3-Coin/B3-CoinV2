// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! FlowMesh certified-log layer: microblock identity and canonical
//! serialization, independent candidate re-execution with MB-0
//! atomicity, BUY/SELL limit intents mapped onto the settled curve
//! economics, chain-bound once-only deposits, Schnorr action
//! credentials, separate certificates with an explicit fault-model
//! threshold, and the round/lock leader-recovery guard.

#include <flowmesh/microblock.h>

#include <flowmesh/auth.h>
#include <flowmesh/batch.h>
#include <flowmesh/certificate.h>
#include <flowmesh/clearing.h>
#include <flowmesh/deposit.h>
#include <flowmesh/recovery.h>
#include <flowmesh/state.h>
#include <key.h>
#include <modern/policy.h>
#include <primitives/transaction.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <map>
#include <optional>
#include <set>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(flowmesh_microblock_tests, BasicTestingSetup)

namespace {

using flowmesh::Action;
using flowmesh::ActionReject;
using flowmesh::ActionType;
using flowmesh::AnchorRef;
using flowmesh::Attestation;
using flowmesh::BatchResult;
using flowmesh::CandidateError;
using flowmesh::CertificateCheck;
using flowmesh::DepositInfo;
using flowmesh::FlowMeshState;
using flowmesh::MicroblockCertificate;
using flowmesh::MicroblockCore;

const uint256 VAULT{uint256{"00000000000000000000000000000000000000000000000000000000000000f1"}};
const uint256 MESH_DOMAIN{uint256{"00000000000000000000000000000000000000000000000000000000000000dd"}};
const uint256 OTHER_DOMAIN{
    uint256{"00000000000000000000000000000000000000000000000000000000000000de"}};
const uint256 DEST{uint256{"00000000000000000000000000000000000000000000000000000000000000d1"}};

modern::AssetId BaseX()
{
    return modern::IssuanceAssetId(COutPoint{
        Txid::FromUint256(uint256{"0000000000000000000000000000000000000000000000000000000000000011"}),
        0});
}
const modern::AssetId& Quote() { return modern::NativeAsset(); }

CKey MakeKey(const unsigned char seed)
{
    std::vector<unsigned char> data(32, seed);
    data[31] = 1; // never all-equal-to-zero-ish patterns that could be invalid
    CKey key;
    key.Set(data.begin(), data.end(), /*fCompressedIn=*/true);
    BOOST_REQUIRE(key.IsValid());
    return key;
}

XOnlyPubKey Xonly(const CKey& key) { return XOnlyPubKey{key.GetPubKey()}; }

COutPoint Outpoint(const unsigned char tag, const uint32_t n)
{
    std::vector<unsigned char> data(32, tag);
    return COutPoint{Txid::FromUint256(uint256{data}), n};
}

class MapDeposits final : public flowmesh::DepositVerifier
{
public:
    std::map<COutPoint, DepositInfo> entries;
    //! Deposits are judged at an anchor: this mock refuses anchors it
    //! was not configured for, like a real verifier refusing a
    //! non-canonical B3 position.
    std::optional<AnchorRef> required_anchor;

    std::optional<DepositInfo> GetDeposit(const COutPoint& outpoint,
                                          const AnchorRef& anchor) const override
    {
        if (required_anchor && !(anchor == *required_anchor)) return std::nullopt;
        const auto it{entries.find(outpoint)};
        if (it == entries.end()) return std::nullopt;
        return it->second;
    }
};

Action Deposit(const COutPoint& outpoint)
{
    Action a;
    a.type = static_cast<uint8_t>(ActionType::DEPOSIT);
    a.outpoint = outpoint;
    return a;
}

Action LimitOrder(const uint256& domain, const CKey& key, const uint64_t seq, const bool buy,
                  const CAmount price, const CAmount qty)
{
    Action a;
    a.signer = flowmesh::AccountForKey(Xonly(key));
    a.sequence = seq;
    a.type = static_cast<uint8_t>(buy ? ActionType::SUBMIT_BID : ActionType::SUBMIT_ASK);
    const auto curve{buy ? flowmesh::MakeLimitBidCurve(price, qty)
                         : flowmesh::MakeLimitAskCurve(price, qty)};
    BOOST_REQUIRE(curve.has_value());
    a.curve = *curve;
    BOOST_REQUIRE(flowmesh::SignAction(key, domain, a));
    return a;
}

struct Net {
    CKey alice_key{MakeKey(0xa1)};
    CKey bob_key{MakeKey(0xb2)};
    flowmesh::AccountId alice{flowmesh::AccountForKey(Xonly(alice_key))};
    flowmesh::AccountId bob{flowmesh::AccountForKey(Xonly(bob_key))};
    flowmesh::SchnorrActionAuthenticator auth{MESH_DOMAIN};
    MapDeposits deposits;
    AnchorRef anchor{100, uint256{"00000000000000000000000000000000000000000000000000000000000000aa"}};
    FlowMeshState state{VAULT, BaseX(), Quote()};

    Net()
    {
        deposits.required_anchor = anchor;
        deposits.entries[Outpoint(0x0a, 0)] = {Quote(), 600'000, alice};
        deposits.entries[Outpoint(0x0b, 1)] = {BaseX(), 10, bob};
    }
};

} // namespace

BOOST_AUTO_TEST_CASE(limit_intents_clear_on_the_curve_economics)
{
    Net net;

    // One microblock: both chain deposits, then Alice BUY 10 @ 50'000
    // and Bob SELL 10 @ 50'000 as degenerate curves.
    std::vector<Action> actions{
        Deposit(Outpoint(0x0a, 0)), Deposit(Outpoint(0x0b, 1)),
        LimitOrder(MESH_DOMAIN, net.alice_key, 0, /*buy=*/true, 50'000, 10),
        LimitOrder(MESH_DOMAIN, net.bob_key, 0, /*buy=*/false, 50'000, 10)};

    FlowMeshState next{net.state};
    BatchResult result;
    const MicroblockCore mb{flowmesh::BuildMicroblock(net.state, MESH_DOMAIN, uint256{}, net.anchor,
                                                      actions, Xonly(net.alice_key), net.auth,
                                                      &net.deposits, next, result)};

    BOOST_REQUIRE(result.clearing.cleared);
    BOOST_CHECK_EQUAL(result.clearing.price, 50'000);
    BOOST_CHECK_EQUAL(result.clearing.volume, 10);
    BOOST_CHECK_EQUAL(result.applied.size(), 4U);
    BOOST_CHECK_EQUAL(next.ledger.Available(net.alice, BaseX()), 10);
    BOOST_CHECK_EQUAL(next.ledger.Available(net.bob, Quote()), 500'000);
    BOOST_CHECK_EQUAL(next.ledger.Available(net.alice, Quote()), 100'000); // 600k - 10*50k
    BOOST_CHECK(next.ledger.SolvencyHolds());

    // An independent replica from the same genesis re-executes to the
    // same state, root and result commitment.
    Net replica;
    FlowMeshState replica_next{replica.state};
    BatchResult replica_result;
    BOOST_REQUIRE(flowmesh::ExecuteCandidate(replica.state, MESH_DOMAIN, uint256{}, mb, replica.auth,
                                             &replica.deposits, replica_next,
                                             replica_result) == CandidateError::NONE);
    BOOST_CHECK_EQUAL(replica_next.Root().GetHex(), next.Root().GetHex());
    BOOST_CHECK_EQUAL(replica_result.result_commitment.GetHex(),
                      result.result_commitment.GetHex());

    // A proposer that lists the same action SET in a different vector
    // order produces a different microblock identity but the identical
    // resulting state: execution order is canonical, not arrival order.
    std::vector<Action> reordered{actions[3], actions[1], actions[0], actions[2]};
    Net third;
    FlowMeshState third_next{third.state};
    BatchResult third_result;
    const MicroblockCore mb2{flowmesh::BuildMicroblock(third.state, MESH_DOMAIN, uint256{},
                                                       third.anchor, reordered,
                                                       Xonly(third.bob_key), third.auth,
                                                       &third.deposits, third_next, third_result)};
    BOOST_CHECK(mb2.GetHash() != mb.GetHash());
    BOOST_CHECK_EQUAL(third_next.Root().GetHex(), next.Root().GetHex());
    BOOST_CHECK_EQUAL(third_result.result_commitment.GetHex(),
                      result.result_commitment.GetHex());
}

BOOST_AUTO_TEST_CASE(candidate_reexecution_rejects_every_tamper_and_stays_atomic)
{
    Net net;
    std::vector<Action> actions{Deposit(Outpoint(0x0a, 0)),
                                LimitOrder(MESH_DOMAIN, net.alice_key, 0, true, 50'000, 4)};
    FlowMeshState built_next{net.state};
    BatchResult built_result;
    const MicroblockCore good{flowmesh::BuildMicroblock(net.state, MESH_DOMAIN, uint256{}, net.anchor,
                                                        actions, Xonly(net.alice_key), net.auth,
                                                        &net.deposits, built_next, built_result)};

    Net replica;
    const uint256 before{replica.state.Root()};
    FlowMeshState out{replica.state};
    BatchResult out_result;
    const auto run{[&](const MicroblockCore& mb) {
        return flowmesh::ExecuteCandidate(replica.state, MESH_DOMAIN, uint256{}, mb, replica.auth,
                                          &replica.deposits, out, out_result);
    }};

    MicroblockCore bad{good};
    bad.version = 2;
    BOOST_CHECK(run(bad) == CandidateError::SHAPE);
    bad = good;
    bad.domain = OTHER_DOMAIN;
    BOOST_CHECK(run(bad) == CandidateError::WRONG_DOMAIN);
    bad = good;
    bad.sequence = 7;
    bad.parent_hash = uint256::ONE; // keep shape valid (nonzero seq wants a parent)
    BOOST_CHECK(run(bad) == CandidateError::SEQUENCE);
    bad = good;
    bad.prev_state_root = uint256::ONE;
    BOOST_CHECK(run(bad) == CandidateError::PREV_ROOT);
    bad = good;
    bad.actions.push_back(Deposit(Outpoint(0x0c, 2)));
    BOOST_CHECK(run(bad) == CandidateError::ACTIONS_ROOT);
    bad = good;
    bad.result_commitment = uint256::ONE;
    BOOST_CHECK(run(bad) == CandidateError::RESULT_COMMITMENT);
    bad = good;
    bad.resulting_state_root = uint256::ONE;
    BOOST_CHECK(run(bad) == CandidateError::RESULT_ROOT);

    // Wrong expected parent (caller-side view).
    BOOST_CHECK(flowmesh::ExecuteCandidate(replica.state, MESH_DOMAIN, uint256::ONE, good,
                                           replica.auth, &replica.deposits, out,
                                           out_result) == CandidateError::PARENT);

    // MB-0: none of the failures touched the committed state.
    BOOST_CHECK_EQUAL(replica.state.Root().GetHex(), before.GetHex());

    // And the honest candidate still lands on the untouched state.
    BOOST_CHECK(run(good) == CandidateError::NONE);
    BOOST_CHECK_EQUAL(out.Root().GetHex(), good.resulting_state_root.GetHex());
}

BOOST_AUTO_TEST_CASE(deposits_come_from_the_chain_and_consume_once)
{
    Net net;

    // Unknown outpoint and missing verifier both fail closed.
    {
        FlowMeshState next{net.state};
        flowmesh::BatchExecutor exec{next, net.auth, &net.deposits};
        const BatchResult r{exec.ExecuteSlot({Deposit(Outpoint(0x0c, 9))}, net.anchor)};
        BOOST_REQUIRE_EQUAL(r.rejected.size(), 1U);
        BOOST_CHECK(r.rejected[0].second == ActionReject::REJECTED_BY_STATE);
    }
    {
        FlowMeshState next{net.state};
        flowmesh::BatchExecutor exec{next, net.auth, /*deposits=*/nullptr};
        const BatchResult r{exec.ExecuteSlot({Deposit(Outpoint(0x0a, 0))}, net.anchor)};
        BOOST_REQUIRE_EQUAL(r.rejected.size(), 1U);
        BOOST_CHECK(r.rejected[0].second == ActionReject::REJECTED_BY_STATE);
    }
    // A deposit judged at the wrong anchor fails: the custody facts are
    // a function of the anchored B3 position, not of the action.
    {
        FlowMeshState next{net.state};
        flowmesh::BatchExecutor exec{next, net.auth, &net.deposits};
        const AnchorRef wrong{net.anchor.height, uint256::ONE};
        const BatchResult r{exec.ExecuteSlot({Deposit(Outpoint(0x0a, 0))}, wrong)};
        BOOST_REQUIRE_EQUAL(r.rejected.size(), 1U);
    }

    // Applied once; the same outpoint can never credit again.
    flowmesh::BatchExecutor exec{net.state, net.auth, &net.deposits};
    const BatchResult first{exec.ExecuteSlot({Deposit(Outpoint(0x0a, 0))}, net.anchor)};
    BOOST_REQUIRE_EQUAL(first.applied.size(), 1U);
    BOOST_CHECK_EQUAL(net.state.ledger.Available(net.alice, Quote()), 600'000);
    const BatchResult second{exec.ExecuteSlot({Deposit(Outpoint(0x0a, 0))}, net.anchor)};
    BOOST_REQUIRE_EQUAL(second.applied.size(), 0U);
    BOOST_REQUIRE_EQUAL(second.rejected.size(), 1U);
    BOOST_CHECK_EQUAL(net.state.ledger.Available(net.alice, Quote()), 600'000);
    BOOST_CHECK(net.state.ledger.SolvencyHolds());
}

BOOST_AUTO_TEST_CASE(schnorr_credentials_bind_signer_and_domain)
{
    Net net;
    net.state.ledger.Deposit(net.alice, Quote(), 600'000);

    Action good{LimitOrder(MESH_DOMAIN, net.alice_key, 0, true, 50'000, 4)};
    BOOST_CHECK(net.auth.Authenticate(good));

    // Corrupted signature byte.
    Action bad{good};
    bad.credential[40] ^= 0x01;
    BOOST_CHECK(!net.auth.Authenticate(bad));

    // Credential from a different key cannot vouch for Alice's account.
    Action stolen{good};
    stolen.credential.clear();
    {
        Action tmp{good};
        tmp.signer = flowmesh::AccountForKey(Xonly(net.bob_key));
        BOOST_REQUIRE(flowmesh::SignAction(net.bob_key, MESH_DOMAIN, tmp));
        stolen.credential = tmp.credential; // Bob's key+sig on Alice-signed action
    }
    BOOST_CHECK(!net.auth.Authenticate(stolen));

    // Wrong-size credential and cross-domain replay both fail.
    Action truncated{good};
    truncated.credential.resize(95);
    BOOST_CHECK(!net.auth.Authenticate(truncated));
    const flowmesh::SchnorrActionAuthenticator other_domain{OTHER_DOMAIN};
    BOOST_CHECK(!other_domain.Authenticate(good));

    // End to end through the executor: the tampered variant is rejected
    // as unauthenticated, the good one applies.
    flowmesh::BatchExecutor exec{net.state, net.auth, nullptr};
    const BatchResult r{exec.ExecuteSlot({bad, good})};
    BOOST_CHECK_EQUAL(r.applied.size(), 1U);
}

BOOST_AUTO_TEST_CASE(certificates_are_separate_thresholded_attestations)
{
    std::vector<CKey> keys;
    std::set<XOnlyPubKey> seats;
    for (unsigned char i{1}; i <= 4; ++i) {
        keys.push_back(MakeKey(i));
        seats.insert(Xonly(keys.back()));
    }
    // Fault model: k=4 seats, f=1 Byzantine -> t=3; k=3f is unservable.
    const auto t{flowmesh::MinCertificateThreshold(4, 1)};
    BOOST_REQUIRE(t.has_value());
    BOOST_CHECK_EQUAL(*t, 3U);
    BOOST_CHECK(!flowmesh::MinCertificateThreshold(3, 1).has_value());
    const auto t10{flowmesh::MinCertificateThreshold(10, 3)};
    BOOST_REQUIRE(t10.has_value());
    BOOST_CHECK_EQUAL(*t10, 7U);

    const uint256 mb_hash{uint256::ONE};
    const uint64_t seq{5};
    std::vector<Attestation> atts;
    for (const CKey& k : keys) {
        const auto a{flowmesh::SignAttestation(k, MESH_DOMAIN, seq, mb_hash)};
        BOOST_REQUIRE(a.has_value());
        atts.push_back(*a);
    }

    // Three of four (assembled from unsorted input, with a duplicate).
    MicroblockCertificate cert{flowmesh::AssembleCertificate(
        mb_hash, seq, {atts[2], atts[0], atts[1], atts[0]})};
    BOOST_CHECK_EQUAL(cert.attestations.size(), 3U);
    BOOST_CHECK(flowmesh::CheckCertificate(cert, MESH_DOMAIN, seats, *t) == CertificateCheck::OK);

    // Below threshold.
    MicroblockCertificate two{flowmesh::AssembleCertificate(mb_hash, seq, {atts[0], atts[1]})};
    BOOST_CHECK(flowmesh::CheckCertificate(two, MESH_DOMAIN, seats, *t) ==
                CertificateCheck::BELOW_THRESHOLD);

    // Outsider attestation.
    const CKey outsider{MakeKey(0x77)};
    const auto oa{flowmesh::SignAttestation(outsider, MESH_DOMAIN, seq, mb_hash)};
    MicroblockCertificate with_outsider{
        flowmesh::AssembleCertificate(mb_hash, seq, {atts[0], atts[1], *oa})};
    BOOST_CHECK(flowmesh::CheckCertificate(with_outsider, MESH_DOMAIN, seats, *t) ==
                CertificateCheck::NOT_A_SEAT);

    // Corrupted signature; non-canonical ordering; wrong domain.
    MicroblockCertificate corrupt{cert};
    corrupt.attestations[1].sig[10] ^= 0x01;
    BOOST_CHECK(flowmesh::CheckCertificate(corrupt, MESH_DOMAIN, seats, *t) ==
                CertificateCheck::BAD_SIGNATURE);
    MicroblockCertificate unsorted{cert};
    std::swap(unsorted.attestations[0], unsorted.attestations[2]);
    BOOST_CHECK(flowmesh::CheckCertificate(unsorted, MESH_DOMAIN, seats, *t) ==
                CertificateCheck::NON_CANONICAL);
    BOOST_CHECK(flowmesh::CheckCertificate(cert, OTHER_DOMAIN, seats, *t) ==
                CertificateCheck::BAD_SIGNATURE);
}

BOOST_AUTO_TEST_CASE(recovery_rounds_and_lock_rule)
{
    std::vector<CKey> keys{MakeKey(0x01), MakeKey(0x02), MakeKey(0x03)};
    std::vector<XOnlyPubKey> seat_keys;
    for (const CKey& k : keys) seat_keys.push_back(Xonly(k));
    const flowmesh::RoundRobinSchedule schedule{seat_keys};

    const uint64_t seq{3};
    const auto p0{schedule.ProposerAt(seq, 0)};
    const auto p1{schedule.ProposerAt(seq, 1)};
    BOOST_REQUIRE(p0 && p1);
    BOOST_CHECK(!(*p0 == *p1));

    flowmesh::AttestationGuard guard;
    const uint256 h1{uint256::ONE};
    const uint256 h2{uint256{"0000000000000000000000000000000000000000000000000000000000000002"}};

    // Round gating: a round-1 proposal is refused while the validator is
    // still in round 0; a wrong proposer is refused in any round.
    BOOST_CHECK(guard.Consider(schedule, seq, 1, *p1, h1) == flowmesh::AttestDecision::WRONG_ROUND);
    BOOST_CHECK(guard.Consider(schedule, seq, 0, *p1, h1) ==
                flowmesh::AttestDecision::WRONG_PROPOSER);
    BOOST_CHECK(guard.Consider(schedule, seq, 0, *p0, h1) == flowmesh::AttestDecision::ATTEST);
    guard.NoteAttested(seq, h1);

    // After a timeout the next round's proposer becomes acceptable — but
    // the lock forbids a DIFFERENT hash forever at this sequence.
    guard.NoteTimeout(seq);
    BOOST_CHECK_EQUAL(guard.CurrentRound(seq), 1U);
    BOOST_CHECK(guard.Consider(schedule, seq, 1, *p1, h2) ==
                flowmesh::AttestDecision::LOCK_CONFLICT);
    BOOST_CHECK(guard.Consider(schedule, seq, 1, *p1, h1) == flowmesh::AttestDecision::ATTEST);

    guard.NoteCertified(seq);
    BOOST_CHECK_EQUAL(guard.CurrentRound(seq), 0U);
    BOOST_CHECK(!guard.LockedHash(seq).has_value());
}

BOOST_AUTO_TEST_CASE(attestation_equivocation_is_detectable)
{
    const CKey key{MakeKey(0x55)};
    const uint64_t seq{9};
    const uint256 h1{uint256::ONE};
    const uint256 h2{uint256{"0000000000000000000000000000000000000000000000000000000000000002"}};

    const auto a1{flowmesh::SignAttestation(key, MESH_DOMAIN, seq, h1)};
    const auto a2{flowmesh::SignAttestation(key, MESH_DOMAIN, seq, h2)};
    BOOST_REQUIRE(a1 && a2);

    const auto evidence{flowmesh::DetectEquivocation(MESH_DOMAIN, seq, h1, *a1, h2, *a2)};
    BOOST_REQUIRE(evidence.has_value());
    BOOST_CHECK_EQUAL(evidence->sequence, seq);

    // Same hash twice is not equivocation; different validators are not
    // equivocation; a forged half is not evidence.
    BOOST_CHECK(!flowmesh::DetectEquivocation(MESH_DOMAIN, seq, h1, *a1, h1, *a1).has_value());
    const CKey other{MakeKey(0x56)};
    const auto b2{flowmesh::SignAttestation(other, MESH_DOMAIN, seq, h2)};
    BOOST_CHECK(!flowmesh::DetectEquivocation(MESH_DOMAIN, seq, h1, *a1, h2, *b2).has_value());
    Attestation forged{*a2};
    forged.sig[5] ^= 0x01;
    BOOST_CHECK(!flowmesh::DetectEquivocation(MESH_DOMAIN, seq, h1, *a1, h2, forged).has_value());
}

BOOST_AUTO_TEST_CASE(canonical_serialization_round_trips)
{
    Net net;
    std::vector<Action> actions{Deposit(Outpoint(0x0a, 0)),
                                LimitOrder(MESH_DOMAIN, net.alice_key, 0, true, 50'000, 4)};
    FlowMeshState next{net.state};
    BatchResult result;
    const MicroblockCore mb{flowmesh::BuildMicroblock(net.state, MESH_DOMAIN, uint256{}, net.anchor,
                                                      actions, Xonly(net.alice_key), net.auth,
                                                      &net.deposits, next, result)};

    DataStream s;
    s << mb;
    MicroblockCore decoded;
    s >> decoded;
    BOOST_CHECK_EQUAL(decoded.GetHash().GetHex(), mb.GetHash().GetHex());
    BOOST_CHECK_EQUAL(decoded.actions.size(), mb.actions.size());
    BOOST_CHECK(decoded.actions[1].Id() == mb.actions[1].Id());

    const CKey key{MakeKey(0x21)};
    const auto att{flowmesh::SignAttestation(key, MESH_DOMAIN, mb.sequence, mb.GetHash())};
    BOOST_REQUIRE(att.has_value());
    const MicroblockCertificate cert{
        flowmesh::AssembleCertificate(mb.GetHash(), mb.sequence, {*att})};
    DataStream s2;
    s2 << cert;
    MicroblockCertificate decoded_cert;
    s2 >> decoded_cert;
    BOOST_CHECK(decoded_cert.microblock_hash == cert.microblock_hash);
    BOOST_CHECK(flowmesh::VerifyAttestation(decoded_cert.attestations[0], MESH_DOMAIN, mb.sequence,
                                            mb.GetHash()));
}

BOOST_AUTO_TEST_SUITE_END()
