// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! FlowMesh certified-log layer: canonical SEMANTIC microblock identity
//! (credential- and permutation-independent), evidence verified outside
//! identity, pre-admission authentication, independent candidate
//! re-execution with MB-0 atomicity, BUY/SELL limit intents on the
//! settled curve economics, chain-bound once-only deposits, separate
//! certificates with an explicit fault-model threshold, and the
//! round/lock leader-recovery guard.

#include <flowmesh/microblock.h>

#include <flowmesh/auth.h>
#include <flowmesh/batch.h>
#include <flowmesh/certificate.h>
#include <flowmesh/clearing.h>
#include <flowmesh/deposit.h>
#include <flowmesh/pool.h>
#include <flowmesh/recovery.h>
#include <flowmesh/state.h>
#include <flowmesh/sync.h>
#include <test/util/flowmesh.h>
#include <key.h>
#include <modern/policy.h>
#include <primitives/transaction.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
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
const uint256 MESH_DOMAIN{
    uint256{"00000000000000000000000000000000000000000000000000000000000000dd"}};
const uint256 MESH_CONFIG{flowmesh::ComputeExecutionConfigId(
    uint256{"00000000000000000000000000000000000000000000000000000000000000f1"},
    modern::IssuanceAssetId(COutPoint{
        Txid::FromUint256(uint256{"0000000000000000000000000000000000000000000000000000000000000011"}),
        0}),
    modern::NativeAsset(), 8)};
const uint256 OTHER_DOMAIN{
    uint256{"00000000000000000000000000000000000000000000000000000000000000de"}};

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
    data[31] = 1;
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
    BOOST_REQUIRE(flowmesh::SignAction(key, domain, MESH_CONFIG, a));
    return a;
}

struct Net {
    CKey alice_key{MakeKey(0xa1)};
    CKey bob_key{MakeKey(0xb2)};
    flowmesh::AccountId alice{flowmesh::AccountForKey(Xonly(alice_key))};
    flowmesh::AccountId bob{flowmesh::AccountForKey(Xonly(bob_key))};
    flowmesh::SchnorrActionAuthenticator auth{MESH_DOMAIN, MESH_CONFIG};
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

struct Built {
    MicroblockCore mb;
    std::vector<std::vector<unsigned char>> credentials;
};

Built MustBuild(Net& net, const std::vector<Action>& actions, FlowMeshState& next,
                BatchResult& result)
{
    Built built;
    const std::optional<MicroblockCore> mb{
        flowmesh::BuildMicroblock(net.state, MESH_DOMAIN, uint256{}, net.anchor, actions,
                                  &net.deposits, next, result, built.credentials)};
    BOOST_REQUIRE(mb.has_value());
    built.mb = *mb;
    return built;
}


//! Test funding shortcut over the test-only bridge.
inline bool Fund(flowmesh::FlowMeshState& state, const flowmesh::AccountId& account,
                 const modern::AssetId& asset, const CAmount amount)
{
    return flowmesh::test_only::StateFunding::Fund(state, account, asset, amount);
}

} // namespace

BOOST_AUTO_TEST_CASE(limit_intents_clear_on_the_curve_economics)
{
    Net net;

    std::vector<Action> actions{
        Deposit(Outpoint(0x0a, 0)), Deposit(Outpoint(0x0b, 1)),
        LimitOrder(MESH_DOMAIN, net.alice_key, 0, /*buy=*/true, 50'000, 10),
        LimitOrder(MESH_DOMAIN, net.bob_key, 0, /*buy=*/false, 50'000, 10)};

    FlowMeshState next{net.state};
    BatchResult result;
    const Built built{MustBuild(net, actions, next, result)};

    BOOST_REQUIRE(result.clearing.cleared);
    BOOST_CHECK_EQUAL(result.clearing.price, 50'000);
    BOOST_CHECK_EQUAL(result.clearing.volume, 10);
    BOOST_CHECK_EQUAL(result.applied.size(), 4U);
    BOOST_CHECK_EQUAL(next.LedgerView().Available(net.alice, BaseX()), 10);
    BOOST_CHECK_EQUAL(next.LedgerView().Available(net.bob, Quote()), 500'000);
    BOOST_CHECK_EQUAL(next.LedgerView().Available(net.alice, Quote()), 100'000); // 600k - 10*50k
    BOOST_CHECK(next.LedgerView().SolvencyHolds());

    // The admitted evidence authenticates the body — and an independent
    // replica re-executes to the same state, root and result commitment.
    BOOST_CHECK(flowmesh::VerifyActionEvidence(built.mb, built.credentials, net.auth));
    Net replica;
    FlowMeshState replica_next{replica.state};
    BatchResult replica_result;
    BOOST_REQUIRE(flowmesh::ExecuteCandidate(replica.state, MESH_DOMAIN, uint256{}, built.mb,
                                             &replica.deposits, replica_next,
                                             replica_result) == CandidateError::NONE);
    BOOST_CHECK_EQUAL(replica_next.Root().GetHex(), next.Root().GetHex());
    BOOST_CHECK_EQUAL(replica_result.result_commitment.GetHex(),
                      result.result_commitment.GetHex());
}

BOOST_AUTO_TEST_CASE(buy_funded_with_exactly_the_notional_is_accepted)
{
    // The degenerate BUY {(P,Q),(P+1,0)} reserves exactly Q×P; a buyer
    // holding exactly the notional succeeds. (The general multi-segment
    // staircase bound is documented as SAFE AND CONSERVATIVE, exact for
    // this degenerate mapping — no broader exactness is claimed.)
    Net net;
    net.deposits.entries[Outpoint(0x0c, 2)] = {Quote(), 500'000, net.alice};
    FlowMeshState next{net.state};
    BatchResult result;
    (void)MustBuild(net,
                    {Deposit(Outpoint(0x0c, 2)),
                     LimitOrder(MESH_DOMAIN, net.alice_key, 0, true, 50'000, 10)},
                    next, result);
    BOOST_CHECK_EQUAL(result.applied.size(), 2U);
    BOOST_CHECK_EQUAL(result.rejected.size(), 0U);
    BOOST_CHECK_EQUAL(next.LedgerView().Reserved(net.alice, Quote()), 500'000);
    BOOST_CHECK_EQUAL(next.LedgerView().Available(net.alice, Quote()), 0);
    BOOST_CHECK(next.LedgerView().SolvencyHolds());
}

BOOST_AUTO_TEST_CASE(microblock_identity_is_semantic_and_permutation_independent)
{
    // Codex re-audit item 1: one logical action set => one canonical
    // body => one hash — across every arrival permutation AND with
    // valid/malformed/alternate credential variants mixed in. The body
    // is credential-free, so evidence can never split identity.
    Net net;
    const std::vector<Action> base{
        Deposit(Outpoint(0x0a, 0)), Deposit(Outpoint(0x0b, 1)),
        LimitOrder(MESH_DOMAIN, net.alice_key, 0, true, 50'000, 10),
        LimitOrder(MESH_DOMAIN, net.bob_key, 0, false, 50'000, 10)};

    // Credential variants of the SAME semantic actions.
    Action alice_corrupt{base[2]};
    alice_corrupt.credential[40] ^= 0x01;
    Action bob_stripped{base[3]};
    bob_stripped.credential.clear();

    std::optional<uint256> pinned_hash;
    std::optional<uint256> pinned_root;
    std::vector<size_t> idx{0, 1, 2, 3};
    do {
        std::vector<Action> permuted;
        for (const size_t i : idx) permuted.push_back(base[i]);
        permuted.push_back(base[idx[0]]); // exact duplicate
        permuted.push_back(alice_corrupt); // variant of an existing id
        permuted.push_back(bob_stripped);  // another variant

        Net fresh;
        FlowMeshState next{fresh.state};
        BatchResult result;
        const Built built{MustBuild(fresh, permuted, next, result)};
        BOOST_REQUIRE(built.mb.ShapeIsValid());
        BOOST_CHECK_EQUAL(built.mb.actions.size(), 4U); // variants/dups collapsed
        for (const Action& body_action : built.mb.actions) {
            BOOST_CHECK(body_action.credential.empty()); // body is evidence-free
        }
        if (!pinned_hash) {
            pinned_hash = built.mb.GetHash();
            pinned_root = built.mb.resulting_state_root;
        } else {
            BOOST_CHECK_EQUAL(built.mb.GetHash().GetHex(), pinned_hash->GetHex());
            BOOST_CHECK_EQUAL(built.mb.resulting_state_root.GetHex(), pinned_root->GetHex());
        }
    } while (std::next_permutation(idx.begin(), idx.end()));

    // A validator refuses any NON-canonical body outright — including a
    // body that smuggles a credential.
    Net fresh;
    FlowMeshState next{fresh.state};
    BatchResult result;
    Built built{MustBuild(fresh, base, next, result)};
    {
        MicroblockCore bad{built.mb};
        std::swap(bad.actions[0], bad.actions[2]);
        bad.actions_root = MicroblockCore::ComputeActionsRoot(bad.actions);
        Net validator;
        FlowMeshState out{validator.state};
        BatchResult out_result;
        BOOST_CHECK(flowmesh::ExecuteCandidate(validator.state, MESH_DOMAIN, uint256{}, bad,
                                               &validator.deposits, out,
                                               out_result) == CandidateError::SHAPE);
    }
    {
        MicroblockCore bad{built.mb};
        bad.actions.back().credential = {0x01}; // evidence inside the body: refused
        bad.actions_root = MicroblockCore::ComputeActionsRoot(bad.actions);
        Net validator;
        FlowMeshState out{validator.state};
        BatchResult out_result;
        BOOST_CHECK(flowmesh::ExecuteCandidate(validator.state, MESH_DOMAIN, uint256{}, bad,
                                               &validator.deposits, out,
                                               out_result) == CandidateError::SHAPE);
    }
}

BOOST_AUTO_TEST_CASE(evidence_lives_outside_identity_and_must_authenticate)
{
    Net net;
    FlowMeshState next{net.state};
    BatchResult result;
    const Built built{MustBuild(
        net, {Deposit(Outpoint(0x0a, 0)), LimitOrder(MESH_DOMAIN, net.alice_key, 0, true,
                                                     50'000, 4)},
        next, result)};
    BOOST_REQUIRE_EQUAL(built.credentials.size(), 1U); // one signed action

    BOOST_CHECK(flowmesh::VerifyActionEvidence(built.mb, built.credentials, net.auth));

    // Corrupted evidence fails; identity is untouched either way.
    auto corrupted{built.credentials};
    corrupted[0][40] ^= 0x01;
    BOOST_CHECK(!flowmesh::VerifyActionEvidence(built.mb, corrupted, net.auth));
    // Oversized evidence fails before any cryptography.
    auto oversized{built.credentials};
    oversized[0].assign(flowmesh::MAX_ACTION_CREDENTIAL_SIZE + 1, 0x5a);
    BOOST_CHECK(!flowmesh::VerifyActionEvidence(built.mb, oversized, net.auth));
    // Count mismatches fail: missing and dangling evidence alike.
    BOOST_CHECK(!flowmesh::VerifyActionEvidence(built.mb, {}, net.auth));
    auto dangling{built.credentials};
    dangling.push_back({0x01});
    BOOST_CHECK(!flowmesh::VerifyActionEvidence(built.mb, dangling, net.auth));
    // Wrong-domain evidence fails (replay across domains).
    const flowmesh::SchnorrActionAuthenticator other{OTHER_DOMAIN, MESH_CONFIG};
    BOOST_CHECK(!flowmesh::VerifyActionEvidence(built.mb, built.credentials, other));
}

BOOST_AUTO_TEST_CASE(pool_authenticates_before_index_ownership)
{
    // Codex re-audit item 2: a junk credential must not reserve the
    // authoritative (signer, sequence) slot — the victim's legitimate
    // action still admits afterwards.
    Net net;
    flowmesh::ActionPool pool{&net.auth};

    const Action victim{LimitOrder(MESH_DOMAIN, net.alice_key, 0, true, 50'000, 4)};
    Action poison{victim};
    poison.credential[40] ^= 0x01; // same (signer, sequence), junk credential
    Action forged_other{LimitOrder(MESH_DOMAIN, net.alice_key, 0, true, 40'000, 2)};
    forged_other.credential = std::vector<unsigned char>(96, 0x00); // garbage credential

    BOOST_CHECK(!pool.Add(poison));       // refused BEFORE index ownership
    BOOST_CHECK(!pool.Add(forged_other)); // ditto for a different forged id
    BOOST_CHECK_EQUAL(pool.Size(), 0U);
    BOOST_CHECK_EQUAL(pool.Bytes(), 0U);
    BOOST_CHECK(pool.Add(victim)); // the legitimate action is NOT shadowed
    BOOST_CHECK_EQUAL(pool.Size(), 1U);

    // A pool without an authenticator fails closed for signed actions
    // but still admits chain-authorized deposits.
    flowmesh::ActionPool no_auth;
    BOOST_CHECK(!no_auth.Add(victim));
    BOOST_CHECK(no_auth.Add(Deposit(Outpoint(0x0a, 0))));
}

BOOST_AUTO_TEST_CASE(state_root_excludes_execution_metadata)
{
    // Persistent state (and its root) must be identical whether or not
    // an unrelated no-op rejection rode along; only the
    // ExecutionResultCommitment may differ.
    Net net;
    Fund(net.state, net.alice, Quote(), 600'000);
    const std::vector<Action> valid{LimitOrder(MESH_DOMAIN, net.alice_key, 0, true, 50'000, 4)};

    // A wrong-sequence action: recorded as BAD_SEQUENCE (metadata only),
    // consumes nothing, changes no state.
    const Action noop{LimitOrder(MESH_DOMAIN, net.bob_key, 5, false, 40'000, 3)};

    FlowMeshState state_a{net.state};
    flowmesh::BatchExecutor exec_a{state_a};
    const auto a{exec_a.ExecuteSlot(valid)};
    BOOST_REQUIRE(a.has_value());

    FlowMeshState state_b{net.state};
    flowmesh::BatchExecutor exec_b{state_b};
    std::vector<Action> with_noop{valid};
    with_noop.push_back(noop);
    const auto b{exec_b.ExecuteSlot(with_noop)};
    BOOST_REQUIRE(b.has_value());

    BOOST_CHECK_EQUAL(state_a.Root().GetHex(), state_b.Root().GetHex());
    BOOST_CHECK_EQUAL(a->state_root.GetHex(), b->state_root.GetHex());
    BOOST_CHECK(a->result_commitment != b->result_commitment);
}

BOOST_AUTO_TEST_CASE(candidate_reexecution_rejects_every_tamper_and_stays_atomic)
{
    Net net;
    std::vector<Action> actions{Deposit(Outpoint(0x0a, 0)),
                                LimitOrder(MESH_DOMAIN, net.alice_key, 0, true, 50'000, 4)};
    FlowMeshState built_next{net.state};
    BatchResult built_result;
    const Built built{MustBuild(net, actions, built_next, built_result)};
    const MicroblockCore good{built.mb};

    Net replica;
    const uint256 before{replica.state.Root()};
    FlowMeshState out{replica.state};
    BatchResult out_result;
    const auto run{[&](const MicroblockCore& mb) {
        return flowmesh::ExecuteCandidate(replica.state, MESH_DOMAIN, uint256{}, mb,
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
    bad.parent_hash = uint256::ONE;
    BOOST_CHECK(run(bad) == CandidateError::SEQUENCE);
    bad = good;
    bad.prev_state_root = uint256::ONE;
    BOOST_CHECK(run(bad) == CandidateError::PREV_ROOT);
    bad = good;
    bad.actions = flowmesh::CanonicalizeActions(
        {good.actions[0], good.actions[1], Deposit(Outpoint(0x0c, 2))});
    BOOST_CHECK(run(bad) == CandidateError::ACTIONS_ROOT);
    bad = good;
    bad.result_commitment = uint256::ONE;
    BOOST_CHECK(run(bad) == CandidateError::RESULT_COMMITMENT);
    bad = good;
    bad.resulting_state_root = uint256::ONE;
    BOOST_CHECK(run(bad) == CandidateError::RESULT_ROOT);

    BOOST_CHECK(flowmesh::ExecuteCandidate(replica.state, MESH_DOMAIN, uint256::ONE, good,
                                           &replica.deposits, out,
                                           out_result) == CandidateError::PARENT);

    // MB-0: none of the failures touched the committed state.
    BOOST_CHECK_EQUAL(replica.state.Root().GetHex(), before.GetHex());

    BOOST_CHECK(run(good) == CandidateError::NONE);
    BOOST_CHECK_EQUAL(out.Root().GetHex(), good.resulting_state_root.GetHex());
}

BOOST_AUTO_TEST_CASE(deposits_come_from_the_chain_and_consume_once)
{
    Net net;

    {
        FlowMeshState next{net.state};
        flowmesh::BatchExecutor exec{next, &net.deposits};
        const auto r{exec.ExecuteSlot({Deposit(Outpoint(0x0c, 9))}, net.anchor)};
        BOOST_REQUIRE(r.has_value());
        BOOST_REQUIRE_EQUAL(r->rejected.size(), 1U);
        BOOST_CHECK(r->rejected[0].second == ActionReject::REJECTED_BY_STATE);
    }
    {
        FlowMeshState next{net.state};
        flowmesh::BatchExecutor exec{next, /*deposits=*/nullptr};
        const auto r{exec.ExecuteSlot({Deposit(Outpoint(0x0a, 0))}, net.anchor)};
        BOOST_REQUIRE(r.has_value());
        BOOST_REQUIRE_EQUAL(r->rejected.size(), 1U);
        BOOST_CHECK(r->rejected[0].second == ActionReject::REJECTED_BY_STATE);
    }
    {
        FlowMeshState next{net.state};
        flowmesh::BatchExecutor exec{next, &net.deposits};
        const AnchorRef wrong{net.anchor.height, uint256::ONE};
        const auto r{exec.ExecuteSlot({Deposit(Outpoint(0x0a, 0))}, wrong)};
        BOOST_REQUIRE(r.has_value());
        BOOST_REQUIRE_EQUAL(r->rejected.size(), 1U);
    }

    flowmesh::BatchExecutor exec{net.state, &net.deposits};
    const auto first{exec.ExecuteSlot({Deposit(Outpoint(0x0a, 0))}, net.anchor)};
    BOOST_REQUIRE(first.has_value());
    BOOST_REQUIRE_EQUAL(first->applied.size(), 1U);
    BOOST_CHECK_EQUAL(net.state.LedgerView().Available(net.alice, Quote()), 600'000);
    const auto second{exec.ExecuteSlot({Deposit(Outpoint(0x0a, 0))}, net.anchor)};
    BOOST_REQUIRE(second.has_value());
    BOOST_REQUIRE_EQUAL(second->applied.size(), 0U);
    BOOST_REQUIRE_EQUAL(second->rejected.size(), 1U);
    BOOST_CHECK_EQUAL(net.state.LedgerView().Available(net.alice, Quote()), 600'000);
    BOOST_CHECK(net.state.LedgerView().SolvencyHolds());
}

BOOST_AUTO_TEST_CASE(schnorr_credentials_bind_signer_and_domain)
{
    Net net;

    Action good{LimitOrder(MESH_DOMAIN, net.alice_key, 0, true, 50'000, 4)};
    BOOST_CHECK(net.auth.Authenticate(good));

    Action bad{good};
    bad.credential[40] ^= 0x01;
    BOOST_CHECK(!net.auth.Authenticate(bad));

    Action stolen{good};
    stolen.credential.clear();
    {
        Action tmp{good};
        tmp.signer = flowmesh::AccountForKey(Xonly(net.bob_key));
        BOOST_REQUIRE(flowmesh::SignAction(net.bob_key, MESH_DOMAIN, MESH_CONFIG, tmp));
        stolen.credential = tmp.credential;
    }
    BOOST_CHECK(!net.auth.Authenticate(stolen));

    Action truncated{good};
    truncated.credential.resize(95);
    BOOST_CHECK(!net.auth.Authenticate(truncated));
    const flowmesh::SchnorrActionAuthenticator other_domain{OTHER_DOMAIN, MESH_CONFIG};
    BOOST_CHECK(!other_domain.Authenticate(good));
}

BOOST_AUTO_TEST_CASE(certificates_are_separate_thresholded_attestations)
{
    std::vector<CKey> keys;
    std::set<XOnlyPubKey> seats;
    for (unsigned char i{1}; i <= 4; ++i) {
        keys.push_back(MakeKey(i));
        seats.insert(Xonly(keys.back()));
    }
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

    MicroblockCertificate cert{flowmesh::AssembleCertificate(
        mb_hash, seq, {atts[2], atts[0], atts[1], atts[0]})};
    BOOST_CHECK_EQUAL(cert.attestations.size(), 3U);
    BOOST_CHECK(flowmesh::CheckCertificate(cert, MESH_DOMAIN, seats, *t) ==
                CertificateCheck::OK);

    BOOST_CHECK(!flowmesh::ValidQuorumConfig(4, 0));
    BOOST_CHECK(!flowmesh::ValidQuorumConfig(0, 1));
    BOOST_CHECK(!flowmesh::ValidQuorumConfig(4, 5));
    BOOST_CHECK(flowmesh::CheckCertificate(cert, MESH_DOMAIN, seats, 0) ==
                CertificateCheck::BAD_QUORUM_CONFIG);
    BOOST_CHECK(flowmesh::CheckCertificate(cert, MESH_DOMAIN, {}, 1) ==
                CertificateCheck::BAD_QUORUM_CONFIG);
    BOOST_CHECK(flowmesh::CheckCertificate(cert, MESH_DOMAIN, seats, 5) ==
                CertificateCheck::BAD_QUORUM_CONFIG);

    MicroblockCertificate two{flowmesh::AssembleCertificate(mb_hash, seq, {atts[0], atts[1]})};
    BOOST_CHECK(flowmesh::CheckCertificate(two, MESH_DOMAIN, seats, *t) ==
                CertificateCheck::BELOW_THRESHOLD);

    const CKey outsider{MakeKey(0x77)};
    const auto oa{flowmesh::SignAttestation(outsider, MESH_DOMAIN, seq, mb_hash)};
    MicroblockCertificate with_outsider{
        flowmesh::AssembleCertificate(mb_hash, seq, {atts[0], atts[1], *oa})};
    BOOST_CHECK(flowmesh::CheckCertificate(with_outsider, MESH_DOMAIN, seats, *t) ==
                CertificateCheck::NOT_A_SEAT);

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

    BOOST_CHECK(guard.Consider(schedule, seq, 1, *p1, h1) == flowmesh::AttestDecision::WRONG_ROUND);
    BOOST_CHECK(guard.Consider(schedule, seq, 0, *p1, h1) ==
                flowmesh::AttestDecision::WRONG_PROPOSER);
    BOOST_CHECK(guard.Consider(schedule, seq, 0, *p0, h1) == flowmesh::AttestDecision::ATTEST);
    guard.NoteAttested(seq, h1);

    guard.NoteTimeout(seq);
    BOOST_CHECK_EQUAL(guard.CurrentRound(seq), 1U);
    BOOST_CHECK(guard.Consider(schedule, seq, 1, *p1, h2) ==
                flowmesh::AttestDecision::LOCK_CONFLICT);
    BOOST_CHECK(guard.Consider(schedule, seq, 1, *p1, h1) == flowmesh::AttestDecision::ATTEST);

    flowmesh::AttestationGuard restarted;
    restarted.ImportLocks(guard.Locks());
    BOOST_CHECK(restarted.Consider(schedule, seq, 0, *p0, h2) ==
                flowmesh::AttestDecision::LOCK_CONFLICT);
    BOOST_CHECK(restarted.Consider(schedule, seq, 0, *p0, h1) ==
                flowmesh::AttestDecision::ATTEST);

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

    BOOST_CHECK(!flowmesh::DetectEquivocation(MESH_DOMAIN, seq, h1, *a1, h1, *a1).has_value());
    const CKey other{MakeKey(0x56)};
    const auto b2{flowmesh::SignAttestation(other, MESH_DOMAIN, seq, h2)};
    BOOST_CHECK(!flowmesh::DetectEquivocation(MESH_DOMAIN, seq, h1, *a1, h2, *b2).has_value());
    Attestation forged{*a2};
    forged.sig[5] ^= 0x01;
    BOOST_CHECK(!flowmesh::DetectEquivocation(MESH_DOMAIN, seq, h1, *a1, h2, forged).has_value());
}

BOOST_AUTO_TEST_CASE(canonical_serialization_round_trips_strictly)
{
    Net net;
    std::vector<Action> actions{Deposit(Outpoint(0x0a, 0)),
                                LimitOrder(MESH_DOMAIN, net.alice_key, 0, true, 50'000, 4)};
    FlowMeshState next{net.state};
    BatchResult result;
    const Built built{MustBuild(net, actions, next, result)};

    DataStream s;
    s << built.mb;
    MicroblockCore decoded;
    s >> decoded;
    BOOST_CHECK(s.empty()); // full consumption
    BOOST_CHECK_EQUAL(decoded.GetHash().GetHex(), built.mb.GetHash().GetHex());
    BOOST_CHECK_EQUAL(decoded.actions.size(), built.mb.actions.size());

    // Attacker-sized counts are refused BEFORE allocation.
    {
        DataStream bad;
        bad << flowmesh::MICROBLOCK_VERSION_V1 << MESH_DOMAIN << uint64_t{0} << uint256{}
            << net.anchor << uint256{};
        WriteCompactSize(bad, flowmesh::MAX_MICROBLOCK_ACTIONS + 1);
        MicroblockCore refuse;
        BOOST_CHECK_THROW(bad >> refuse, std::ios_base::failure);
    }
    {
        DataStream bad;
        bad << net.alice << uint64_t{0} << uint8_t{0};
        WriteCompactSize(bad, flowmesh::MAX_ACTION_CURVE_POINTS + 1);
        Action refuse;
        BOOST_CHECK_THROW(bad >> refuse, std::ios_base::failure);
    }
    {
        DataStream bad;
        bad << uint256::ONE << uint64_t{1};
        WriteCompactSize(bad, flowmesh::MAX_CERTIFICATE_ATTESTATIONS + 1);
        MicroblockCertificate refuse;
        BOOST_CHECK_THROW(bad >> refuse, std::ios_base::failure);
    }
    // Evidence codec bounds: an oversized evidence list and an oversized
    // single credential both throw before allocation.
    {
        DataStream bad;
        WriteCompactSize(bad, flowmesh::MAX_MICROBLOCK_ACTIONS + 1);
        std::vector<std::vector<unsigned char>> refuse;
        BOOST_CHECK_THROW(flowmesh::UnserializeEvidence(bad, refuse), std::ios_base::failure);
    }
    {
        DataStream bad;
        WriteCompactSize(bad, 1);
        WriteCompactSize(bad, flowmesh::MAX_ACTION_CREDENTIAL_SIZE + 1);
        std::vector<std::vector<unsigned char>> refuse;
        BOOST_CHECK_THROW(flowmesh::UnserializeEvidence(bad, refuse), std::ios_base::failure);
    }

    // A certified entry round-trips with its evidence intact.
    const CKey key{MakeKey(0x21)};
    const auto att{flowmesh::SignAttestation(key, MESH_DOMAIN, built.mb.sequence,
                                             built.mb.GetHash())};
    BOOST_REQUIRE(att.has_value());
    flowmesh::CertifiedEntry entry{built.mb,
                                   flowmesh::AssembleCertificate(built.mb.GetHash(),
                                                                 built.mb.sequence, {*att}),
                                   built.credentials};
    DataStream s2;
    s2 << entry;
    flowmesh::CertifiedEntry decoded_entry;
    s2 >> decoded_entry;
    BOOST_CHECK(s2.empty());
    BOOST_CHECK(decoded_entry.mb.GetHash() == built.mb.GetHash());
    BOOST_CHECK(decoded_entry.credentials == built.credentials);
    BOOST_CHECK(flowmesh::VerifyActionEvidence(decoded_entry.mb, decoded_entry.credentials,
                                               net.auth));
}

BOOST_AUTO_TEST_CASE(authorization_is_bound_to_the_execution_configuration)
{
    // Codex item 1: an authorization signed for market/config A must be
    // rejected under market/config B — cross-market replay and
    // substitution are impossible — and a foreign configuration's
    // candidate is refused structurally (different state roots).
    Net net; // market A: VAULT / BaseX / native quote
    const modern::AssetId other_quote{modern::IssuanceAssetId(COutPoint{
        Txid::FromUint256(uint256{"0000000000000000000000000000000000000000000000000000000000000022"}),
        0})};
    const uint256 config_b{flowmesh::ComputeExecutionConfigId(VAULT, BaseX(), other_quote, 8)};
    BOOST_CHECK(config_b != MESH_CONFIG);
    BOOST_CHECK(net.state.ConfigId() == MESH_CONFIG);

    const Action a_action{LimitOrder(MESH_DOMAIN, net.alice_key, 0, true, 50'000, 4)};
    const flowmesh::SchnorrActionAuthenticator auth_b{MESH_DOMAIN, config_b};
    BOOST_CHECK(net.auth.Authenticate(a_action));  // valid in its own market
    BOOST_CHECK(!auth_b.Authenticate(a_action));   // dead in any other market

    Fund(net.state, net.alice, Quote(), 600'000);
    FlowMeshState next{net.state};
    BatchResult result;
    const Built built{MustBuild(net, {a_action}, next, result)};
    BOOST_CHECK(flowmesh::VerifyActionEvidence(built.mb, built.credentials, net.auth));
    BOOST_CHECK(!flowmesh::VerifyActionEvidence(built.mb, built.credentials, auth_b));

    // A different-config state refuses the candidate before execution:
    // the config lives in the state roots.
    FlowMeshState state_b{VAULT, BaseX(), other_quote};
    BOOST_CHECK(state_b.ConfigId() == config_b);
    FlowMeshState out{state_b};
    BatchResult out_result;
    BOOST_CHECK(flowmesh::ExecuteCandidate(state_b, MESH_DOMAIN, uint256{}, built.mb, nullptr,
                                           out, out_result) == CandidateError::PREV_ROOT);
}

BOOST_AUTO_TEST_SUITE_END()
