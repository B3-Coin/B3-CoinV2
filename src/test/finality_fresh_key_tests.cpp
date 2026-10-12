// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Bridge V2 B-37 (b3-bridge-v2-finality-dependency.md section 6.4): the fresh
// lineage's own per-epoch key binding kind. Pure layer: the FINALITY_KEY
// policy-version-2 cell, MPA record type 12, the FRESHBIND digest, the
// structure + cryptography check, the cell<->evidence bijection, the per-block
// verifier, and the registry/cost/activation gates. V1 FINALITY_KEY bindings
// stay exactly as they were: the two matchers never see each other's objects.
// Chain layer (FinalityChainFixture): the mempool and block wiring in
// validation.cpp -- refused exactly as before without R_key, inactive below
// it, structure + cryptography as consensus from it, mined by the unchanged
// assembler and connected.
// Node layer (node/finality_fresh_key_index.h): the ever-bound key index
// (first claim wins across kinds, reuse inert, exact undo, rebuild equality),
// the binding windows, the mempool policy, and the index kept by the
// FinalityTracker in key mode only (V1 fields unchanged).
// Committee layer (node/finality_fresh_lineage.h B-37 seat rules): one seat
// per member and epoch (none sits out, two void it), the windows of the
// genesis and successor committees, the carry-over, the unchanged projection
// without seat rules, and on a chain: committees on fresh keys only, a V1-key
// certificate refused, rotation to new keys, a void seat, reorg and rebuild.
// Store layer (node/finality_fresh_key_store.h): the signer's key store kept
// outside the wallet (random keys, exact file layout, 0600, durable reopen,
// second key per epoch refused, bind txid never changed, erasure leaves no
// secret byte, fail closed on any corrupt, foreign or changed file, the
// record cap) and the erasure policy truth table.
// Signer layer (node/finality_signature.h, fresh-key mode): the seat windows
// and the signer's key status (keys, seats without a held secret, alerts for
// a foreign binding and a void seat), and on a chain: offline and persistent
// staging signers vote only with fresh keys (never a V1 key), reload their
// store, erase keys by policy (the genesis key at start_2 + horizon, a void
// key once its committee is fixed and deep, a carried-over key kept), report
// and log alerts once, report key status even when the journal refuses, and
// leave the V1 votes and journal byte-identical.
// Status layer (node::FreshKeyStatusAt): the next block's binding window,
// the bindings marked counted / void, the committees' seats, and the same
// data through getfinalitylineage (fresh_keys, local_signer key fields) and
// interfaces::Chain::freshFinalityKeyStatus -- absent without R_key.

#include <addresstype.h>
#include <chainparams.h>
#include <common/args.h>
#include <consensus/consensus.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <crypto/common.h>
#include <crypto/sha256.h>
#include <crypto/bls.h>
#include <hash.h>
#include <interfaces/chain.h>
#include <key.h>
#include <modern/creation_action.h>
#include <modern/finality_fresh_key.h>
#include <modern/finality_key.h>
#include <modern/finality_types.h>
#include <modern/finality_v2.h>
#include <modern/metadata_cell.h>
#include <modern/mpa.h>
#include <modern/payload_cost.h>
#include <modern/payload_root.h>
#include <modern/policy.h>
#include <modern/finality_schedule.h>
#include <node/blockstorage.h>
#include <node/finality_binding_index.h>
#include <node/finality_fresh_key_index.h>
#include <node/finality_fresh_key_store.h>
#include <node/finality_fresh_lineage.h>
#include <node/finality_fresh_signature.h>
#include <node/finality_signature.h>
#include <node/finality_signer_store.h>
#include <node/finality_tracker.h>
#include <node/miner.h>
#include <node/stake_registry.h>
#include <node/validator_set.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <rpc/client.h>
#include <rpc/register.h>
#include <rpc/request.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <script/script.h>
#include <txmempool.h>
#include <uint256.h>
#include <univalue.h>
#include <util/chaintype.h>
#include <util/fs.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <validation.h>

#include <test/util/finality_fixture.h>
#include <test/util/logging.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <vector>

using modern::BlsPubkeyBytes;
using modern::CheckFreshKeyTransition;
using modern::FreshFinalityKeyEvidence;
using modern::FreshFinalityKeyPair;
using modern::FreshFinalityKeyParams;
using modern::FreshKeyCheck;
using modern::PayloadTypeStatus;
using modern::ValidatorKeyBytes;

namespace {

const uint256 CHAIN_DOMAIN{uint256{"d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0"}};
const uint256 OTHER_CHAIN_DOMAIN{uint256{"d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1"}};
constexpr uint16_t T12{modern::MPA_TYPE_FRESH_FINALITY_KEY_EVIDENCE};
constexpr uint16_t FINALITY_KEY_POLICY{static_cast<uint16_t>(modern::PolicyType::FINALITY_KEY)};

struct Validator {
    CKey identity;
    ValidatorKeyBytes vk{};
    explicit Validator(const unsigned char seed)
    {
        std::vector<unsigned char> data(32, seed);
        identity.Set(data.begin(), data.end(), true);
        const XOnlyPubKey x{identity.GetPubKey()};
        std::copy(x.data(), x.data() + 32, vk.begin());
    }
    uint256 Commitment() const
    {
        uint256 c;
        std::copy(vk.begin(), vk.end(), c.begin());
        return c;
    }
};

bls::SecretKey BlsKey(const unsigned i)
{
    std::array<unsigned char, 32> ikm{};
    ikm[0] = static_cast<unsigned char>(i);
    ikm[31] = 0x37;
    return *bls::SecretKey::FromIKM(ikm);
}

BlsPubkeyBytes Pk(const bls::SecretKey& sk) { return sk.GetPublicKey().Compressed(); }

struct Built {
    FreshFinalityKeyParams params;
    FreshFinalityKeyEvidence ev;
};

void SignIdentity(Built& b, const uint256& domain, const CKey& signer)
{
    const uint256 digest{modern::FreshFinalityBindDigest(domain, b.ev.validator_key, b.ev.bls_pubkey, b.ev.target_epoch)};
    uint256 aux{};
    BOOST_REQUIRE(signer.SignSchnorr(digest, b.ev.bip340_sig, nullptr, aux));
}

//! The identity-bound proof of possession `sk` gives for (vk, pk, epoch).
std::array<unsigned char, 96> FreshPop(const bls::SecretKey& sk, const uint256& domain, const ValidatorKeyBytes& vk,
                                       const BlsPubkeyBytes& pk, const uint64_t epoch)
{
    const uint256 digest{modern::FreshFinalityPopDigest(domain, vk, pk, epoch)};
    return sk.Sign(std::span<const unsigned char>(digest.begin(), 32)).Compressed();
}

//! Re-sign the evidence's proof of possession with `sk` for its own fields.
void SignPop(Built& b, const uint256& domain, const bls::SecretKey& sk)
{
    b.ev.pop = FreshPop(sk, domain, b.ev.validator_key, b.ev.bls_pubkey, b.ev.target_epoch);
}

//! A fresh cell params + evidence pair for (v, sk, epoch).
Built Build(const Validator& v, const bls::SecretKey& sk, const uint64_t epoch,
            const uint256& domain = CHAIN_DOMAIN, const CKey* signer = nullptr)
{
    Built b;
    b.params.bls_pubkey = Pk(sk);
    b.params.target_epoch = epoch;
    b.ev.validator_key = v.vk;
    b.ev.bls_pubkey = b.params.bls_pubkey;
    b.ev.target_epoch = epoch;
    SignIdentity(b, domain, signer ? *signer : v.identity);
    SignPop(b, domain, sk);
    return b;
}

FreshKeyCheck Check(const Validator& v, const Built& b, const uint256& domain = CHAIN_DOMAIN)
{
    return CheckFreshKeyTransition(domain, v.Commitment(), b.params, b.ev);
}

CMutableTransaction BaseTx()
{
    CMutableTransaction mtx;
    mtx.version = 2;
    mtx.vin.resize(1);
    mtx.vin[0].prevout = COutPoint{Txid::FromUint256(uint256{"2222222222222222222222222222222222222222222222222222222222222222"}), 1};
    mtx.vin[0].scriptSig = CScript() << OP_1;
    mtx.vout.emplace_back(1234, CScript() << OP_TRUE);
    return mtx;
}

CTxOut FreshCell(const Validator& v, const FreshFinalityKeyParams& params)
{
    return CTxOut{0, modern::MakeFreshFinalityKeyCell(v.vk, params)};
}

//! Append one fresh pair (cell + type-12 record) and keep the MPA canonical.
void AddPair(CMutableTransaction& tx, const Validator& v, const Built& b)
{
    tx.vout.push_back(FreshCell(v, b.params));
    tx.mpa.push_back(modern::MakeFreshFinalityKeyRecord(b.ev));
    std::sort(tx.mpa.begin(), tx.mpa.end(), modern::MpaRecordLess);
}

//! A V1 FINALITY_KEY pair (policy version 1 cell + type-5 record), seq 0.
void AddV1Pair(CMutableTransaction& tx, const Validator& v, const bls::SecretKey& sk)
{
    modern::FinalityKeyParams params;
    params.bls_pubkey = Pk(sk);
    params.seq = 0;
    const auto cell{modern::MakeMetadataCellScript(FINALITY_KEY_POLICY, modern::POLICY_VERSION_V1, v.Commitment(),
                                                   params.Encode())};
    BOOST_REQUIRE(cell);
    tx.vout.emplace_back(0, *cell);
    modern::FinalityKeyEvidence ev;
    ev.validator_key = v.vk;
    ev.bls_pubkey = params.bls_pubkey;
    ev.seq = 0;
    const uint256 digest{modern::FinalityBindDigest(CHAIN_DOMAIN, ev.validator_key, ev.bls_pubkey, 0)};
    uint256 aux{};
    BOOST_REQUIRE(v.identity.SignSchnorr(digest, ev.bip340_sig, nullptr, aux));
    ev.pop = sk.SignPoP().Compressed();
    CMpaRecord rec;
    rec.payload_type = modern::MPA_TYPE_FINALITY_KEY_EVIDENCE;
    rec.payload_version = modern::MPA_VERSION_V1;
    const auto enc{ev.Encode()};
    rec.payload.assign(enc.begin(), enc.end());
    tx.mpa.push_back(rec);
    std::sort(tx.mpa.begin(), tx.mpa.end(), modern::MpaRecordLess);
}

bool MatchFresh(const CMutableTransaction& tx, std::vector<FreshFinalityKeyPair>& out, std::string& error)
{
    return modern::MatchFreshFinalityKeyPairs(CTransaction{tx}, out, error);
}

//! A V1 FINALITY_KEY revocation pair (zero key) at `seq`: it claims no key.
//! The matcher checks no signature, so the evidence carries none.
void AddV1Revocation(CMutableTransaction& tx, const Validator& v, const uint32_t seq)
{
    modern::FinalityKeyParams params; // zero key
    params.seq = seq;
    const auto cell{modern::MakeMetadataCellScript(FINALITY_KEY_POLICY, modern::POLICY_VERSION_V1, v.Commitment(),
                                                   params.Encode())};
    BOOST_REQUIRE(cell);
    tx.vout.emplace_back(0, *cell);
    modern::FinalityKeyEvidence ev;
    ev.validator_key = v.vk;
    ev.seq = seq;
    CMpaRecord rec;
    rec.payload_type = modern::MPA_TYPE_FINALITY_KEY_EVIDENCE;
    rec.payload_version = modern::MPA_VERSION_V1;
    const auto enc{ev.Encode()};
    rec.payload.assign(enc.begin(), enc.end());
    tx.mpa.push_back(rec);
    std::sort(tx.mpa.begin(), tx.mpa.end(), modern::MpaRecordLess);
}

using FreshClaim = node::FreshKeyBindingIndex::Claim;
FreshClaim V1Claim(const Validator& v, const BlsPubkeyBytes& pk) { return FreshClaim{false, v.vk, 0, pk}; }
FreshClaim NewClaim(const Validator& v, const BlsPubkeyBytes& pk, const uint64_t epoch)
{
    return FreshClaim{true, v.vk, epoch, pk};
}

uint256 SnapHashOf(const std::shared_ptr<const node::ValidatorSetSnapshot>& s) { return s ? s->SetHash() : uint256{}; }

//! Every V1 field of the tracker state (State::fresh excluded).
bool SameV1Fields(const node::FinalityTracker::State& a, const node::FinalityTracker::State& b)
{
    return a.bootstrapped == b.bootstrapped && a.epoch == b.epoch && a.epoch_starts == b.epoch_starts &&
           SnapHashOf(a.bootstrap) == SnapHashOf(b.bootstrap) && SnapHashOf(a.previous) == SnapHashOf(b.previous) &&
           SnapHashOf(a.current) == SnapHashOf(b.current) && SnapHashOf(a.next) == SnapHashOf(b.next) &&
           a.handover_certified == b.handover_certified && a.lineage_broken == b.lineage_broken &&
           a.finalized == b.finalized;
}

//! X-pin configuration (types 4/5 live) without and with a fresh lineage.
struct Configs {
    static constexpr int R_F{262};
    static constexpr int R_KEY{241};
    Consensus::Params production;
    Consensus::Params xpin;
    Consensus::Params fresh;          // fresh lineage, no key height (pre-B-37 staging)
    Consensus::Params keyed;          // fresh lineage with R_key
    Consensus::Params keyed_no_xpin;  // R_key without the X-pin configuration
    Configs()
    {
        production.legacy_b3coin = true;
        xpin = production;
        xpin.hard_fork_height = 100;
        xpin.legacy_final_hash = uint256::ONE;
        xpin.modern_pos = Consensus::ModernPosParams{};
        fresh = xpin;
        fresh.fresh_lineage = Consensus::FreshLineageParams{
            .activation_height = R_F, .max_epoch_extension = 1280, .staging_signer = true};
        keyed = fresh;
        keyed.fresh_lineage->key_height = R_KEY;
        keyed_no_xpin = production;
        keyed_no_xpin.fresh_lineage = keyed.fresh_lineage;
    }
};

//! Chain layer: FinalityChainFixture (V1 validators A and B staked and bound)
//! plus builders for fresh bindings over the fixture's real chain domain.
struct FreshKeyChainFixture : public b3test::FinalityChainFixture {
    struct FreshBinding {
        CScript cell;
        CMpaRecord record;
    };
    //! Fresh cell + type-12 record for (identity, vk, sk, epoch). `signer`
    //! replaces the BIP340 identity signer and `pop_key` the key that signs
    //! the proof of possession (negative cases).
    FreshBinding MakeFreshBinding(const CKey& identity, const ValidatorKeyBytes& vk, const bls::SecretKey& sk,
                                  const uint64_t epoch, const CKey* signer = nullptr,
                                  const bls::SecretKey* pop_key = nullptr)
    {
        FreshFinalityKeyParams params;
        params.bls_pubkey = Pk(sk);
        params.target_epoch = epoch;
        FreshFinalityKeyEvidence ev;
        ev.validator_key = vk;
        ev.bls_pubkey = params.bls_pubkey;
        ev.target_epoch = epoch;
        const uint256 digest{modern::FreshFinalityBindDigest(m_domain, vk, ev.bls_pubkey, epoch)};
        uint256 aux{};
        BOOST_REQUIRE((signer ? *signer : identity).SignSchnorr(digest, ev.bip340_sig, nullptr, aux));
        ev.pop = FreshPop(pop_key ? *pop_key : sk, m_domain, vk, ev.bls_pubkey, epoch);
        return {modern::MakeFreshFinalityKeyCell(vk, params), modern::MakeFreshFinalityKeyRecord(ev)};
    }
    //! Spends funding output `n` with a standard change output and a solid
    //! fee (the fixture's OP_TRUE change is non-standard by design).
    CMutableTransaction RelayTx(const unsigned n, const std::vector<CScript>& cells,
                                const std::vector<CMpaRecord>& records)
    {
        CMutableTransaction tx{MakeTx(n, cells, records)};
        tx.vout[0].nValue -= 1'000'000;
        tx.vout[0].scriptPubKey = GetScriptForDestination(PKHash(m_validator_a.GetPubKey()));
        return tx;
    }
    MempoolAcceptResult Mempool(const CMutableTransaction& tx, const bool test_accept = true)
    {
        LOCK(cs_main);
        return m_node.chainman->ProcessTransaction(MakeTransactionRef(tx), test_accept);
    }
    void CheckMempoolRefused(const CMutableTransaction& tx, const std::string& reason, const std::string& debug = {})
    {
        const auto res{Mempool(tx)};
        BOOST_CHECK(res.m_result_type == MempoolAcceptResult::ResultType::INVALID);
        // Every refusal here is context-free once the height is fixed: the
        // transaction can never be mined, so it is a consensus failure.
        BOOST_CHECK(res.m_state.GetResult() == TxValidationResult::TX_CONSENSUS);
        BOOST_CHECK_EQUAL(res.m_state.GetRejectReason(), reason);
        BOOST_CHECK_EQUAL(res.m_state.GetDebugMessage(), debug);
    }
    //! The next block by A carrying `tx`, mock time advanced to it.
    CBlock NextBlockWith(const CMutableTransaction& tx, const int extra = 0)
    {
        const CBlock block{BuildPosBlock(m_vk_a, {}, {tx}, extra)};
        if (block.GetBlockTime() > GetTime()) SetMockTime(block.GetBlockTime());
        return block;
    }
    //! Full validity of that block on the tip (empty = valid); nothing is stored.
    std::string ProbeWith(const CMutableTransaction& tx) { return Probe(NextBlockWith(tx)); }
    //! Spends output 0 (OP_TRUE) of `prev` with `cells` / `records`; `relay`
    //! gives it a standard change output and a solid fee, like RelayTx.
    CMutableTransaction SpendTx(const CTransaction& prev, const std::vector<CScript>& cells,
                                std::vector<CMpaRecord> records, const bool relay)
    {
        CMutableTransaction tx;
        tx.version = 2;
        tx.vin.resize(1);
        tx.vin[0].prevout = COutPoint{prev.GetHash(), 0};
        CScript change{CScript() << OP_TRUE};
        if (relay) change = GetScriptForDestination(PKHash(m_validator_a.GetPubKey()));
        tx.vout.emplace_back(prev.vout[0].nValue - (relay ? 1'000'000 : 100), change);
        for (const auto& c : cells) tx.vout.emplace_back(0, c);
        std::sort(records.begin(), records.end(), modern::MpaRecordLess);
        tx.mpa = std::move(records);
        return tx;
    }
    //! A mempool refusal by the B-37 node policy (never consensus).
    void CheckPolicyRefused(const CMutableTransaction& tx, const std::string& reason)
    {
        const auto res{Mempool(tx)};
        BOOST_CHECK(res.m_result_type == MempoolAcceptResult::ResultType::INVALID);
        BOOST_CHECK(res.m_state.GetResult() == TxValidationResult::TX_NOT_STANDARD);
        BOOST_CHECK_EQUAL(res.m_state.GetRejectReason(), reason);
    }
    //! A copy of the live tracker's fresh key index at the tip (key mode).
    node::FreshKeyBindingIndex FreshKeysAtTip()
    {
        LOCK(cs_main);
        const node::FreshKeyBindingIndex* index{Finality().FreshKeys(m_node.chainman->GetConsensus())};
        BOOST_REQUIRE(index != nullptr);
        return *index;
    }
};


// ------------------------------------------------------ committee helpers

uint256 FreshSetHashOf(const std::shared_ptr<const node::FreshLineageSet>& s) { return s ? s->set_hash : uint256{}; }

//! Every consensus-derived field of two fresh-lineage states.
bool SameFreshState(const std::optional<node::FreshLineageState>& a, const std::optional<node::FreshLineageState>& b)
{
    if (a.has_value() != b.has_value()) return false;
    if (!a) return true;
    return a->bootstrapped == b->bootstrapped && a->genesis_height == b->genesis_height &&
           a->genesis_block_hash == b->genesis_block_hash && a->lineage_id == b->lineage_id &&
           a->epoch == b->epoch && a->epoch_starts == b->epoch_starts &&
           FreshSetHashOf(a->genesis) == FreshSetHashOf(b->genesis) &&
           FreshSetHashOf(a->previous2) == FreshSetHashOf(b->previous2) &&
           FreshSetHashOf(a->previous) == FreshSetHashOf(b->previous) &&
           FreshSetHashOf(a->current) == FreshSetHashOf(b->current) &&
           FreshSetHashOf(a->next) == FreshSetHashOf(b->next) && a->handover_certified == b->handover_certified &&
           a->lineage_broken == b->lineage_broken && a->finalized == b->finalized && a->history == b->history &&
           a->current_handover == b->current_handover && a->finalized_evidence == b->finalized_evidence &&
           ((!a->handover_history && !b->handover_history) ||
            (a->handover_history && b->handover_history && *a->handover_history == *b->handover_history));
}

//! The key `vk` sits on in `snap` (nullopt when not a member).
std::optional<BlsPubkeyBytes> SnapKey(const node::ValidatorSetSnapshot& snap, const ValidatorKeyBytes& vk)
{
    const auto i{snap.IndexOf(vk)};
    if (!i) return std::nullopt;
    return snap.Members()[*i].bls_pubkey;
}
std::optional<BlsPubkeyBytes> SeatKey(const std::shared_ptr<const node::FreshLineageSet>& set,
                                      const ValidatorKeyBytes& vk)
{
    BOOST_REQUIRE(set && set->members);
    return SnapKey(*set->members, vk);
}
std::optional<BlsPubkeyBytes> Opt(const bls::SecretKey& sk) { return Pk(sk); }

//! FreshKeyChainFixture plus what the B-37 committee cases need: type-11
//! certificates signed by chosen keys, blocks carrying them, chain reorgs,
//! and a V1 reference tracker.
struct FreshCommitteeChainFixture : public FreshKeyChainFixture {
    modern::FreshHashAtFn ChainHashFn()
    {
        return [this](const int h) -> std::optional<uint256> {
            LOCK(cs_main);
            const CBlockIndex* index{m_node.chainman->ActiveChain()[h]};
            if (!index) return std::nullopt;
            return index->GetBlockHash();
        };
    }
    std::optional<node::FreshLineageState> Fresh() { return FinalityState().fresh; }

    //! The FinalizedBlockV2 the next block's projected fresh state expects
    //! for `checkpoint`, in the fresh epoch of the checkpoint.
    modern::FinalizedBlockV2 FreshFbForNext(const int checkpoint)
    {
        const auto projected{ProjectedNext()};
        BOOST_REQUIRE(projected.fresh && projected.fresh->bootstrapped);
        const auto e{modern::EpochOfHeight(projected.fresh->epoch_starts, checkpoint)};
        BOOST_REQUIRE(e.has_value());
        const auto fb{node::ExpectedFinalizedBlockV2(*e, checkpoint, *projected.fresh, ChainHashFn(),
                                                     m_node.chainman->GetConsensus(), nullptr)};
        BOOST_REQUIRE(fb.has_value());
        return *fb;
    }
    //! The committee the next block's projected state assigns to `epoch`.
    std::shared_ptr<const node::FreshLineageSet> SigningSetForNext(const uint64_t epoch)
    {
        const auto projected{ProjectedNext()};
        BOOST_REQUIRE(projected.fresh);
        const auto set{projected.fresh->SetForEpoch(epoch)};
        BOOST_REQUIRE(set);
        return set;
    }
    //! A type-11 record over `fb`: each listed validator signs, at its
    //! position in `signing`, with the listed key (right or wrong).
    CMpaRecord FreshRecord(const modern::FinalizedBlockV2& fb, const node::FreshLineageSet& signing,
                           const std::vector<std::pair<ValidatorKeyBytes, const bls::SecretKey*>>& signers)
    {
        const uint256 digest{modern::FinalityDigestV2(m_domain, fb)};
        modern::FinalityCertificate cert;
        cert.signer_bitmap.assign(modern::SignerBitmapBytes(signing.Size()), 0);
        std::vector<bls::Signature> sigs;
        for (const auto& [vk, sk] : signers) {
            const auto i{signing.IndexOf(vk)};
            BOOST_REQUIRE(i.has_value());
            cert.signer_bitmap[*i / 8] |= static_cast<unsigned char>(1U << (*i % 8));
            sigs.push_back(sk->Sign(std::span<const unsigned char>(digest.begin(), 32)));
        }
        BOOST_REQUIRE(!sigs.empty());
        cert.aggregate_sig = bls::AggregateSignatures(sigs)->Compressed();
        CMpaRecord rec;
        rec.payload_type = modern::MPA_TYPE_FINALITY_CERTIFICATE_V2;
        rec.payload_version = modern::MPA_VERSION_V1;
        rec.payload = modern::EncodeCertificatePayloadV2(fb, cert);
        return rec;
    }
    //! A V1 certificate (cell + record) for `checkpoint`, signed by the set
    //! the next block's projected V1 state assigns to its epoch.
    std::pair<CScript, CMpaRecord> V1CertForNext(const int checkpoint)
    {
        const auto s{ProjectedNext()};
        const auto e{modern::EpochOfHeight(s.epoch_starts, checkpoint)};
        BOOST_REQUIRE(e.has_value());
        const bool delayed{*e != s.epoch};
        BOOST_REQUIRE(delayed ? s.previous != nullptr : s.current != nullptr);
        const auto& signing{delayed ? *s.previous : *s.current};
        const uint256 successor{delayed ? s.current->SetHash() : s.next->SetHash()};
        return MakeCertificate({checkpoint, *e, successor}, signing);
    }
    //! The next block by A with V1 certificate pairs (cell + record) and V2
    //! records (type 11 has no cell) in its coinbase (FinishPosBlock adds a
    //! cell per pair, so the V2 records are added here).
    CBlock BuildFreshBlock(const std::vector<std::pair<CScript, CMpaRecord>>& v1, const std::vector<CMpaRecord>& v2,
                           std::vector<CMutableTransaction> txs = {})
    {
        const modern::PosValidatorKey& key{m_vk_a};
        const CBlockIndex* prev{Tip()};
        const Consensus::ModernPosParams& pos{*m_node.chainman->GetConsensus().modern_pos};
        auto [w, W] = RoundWeights(prev, key);
        BOOST_REQUIRE(w > 0);
        const uint256 seed{SeedFor(prev)};
        int64_t round{-1};
        for (int64_t r{0}; r < 100'000 && round < 0; ++r) {
            const uint256 d{modern::ModernPosEligibilityDigest(Domain(), seed, static_cast<uint32_t>(prev->nHeight + 1),
                                                               static_cast<uint32_t>(r), key)};
            if (modern::ModernPosEligible(d, w, W, r, pos)) round = r;
        }
        BOOST_REQUIRE(round >= 0);
        CBlock block{BuildPos(prev, key, round, 0, 0, std::move(txs))};
        CMutableTransaction coinbase{*block.vtx[0]};
        std::vector<CMpaRecord> records;
        for (const auto& [cell, rec] : v1) {
            coinbase.vout.emplace_back(0, cell);
            records.push_back(rec);
        }
        records.insert(records.end(), v2.begin(), v2.end());
        std::sort(records.begin(), records.end(), modern::MpaRecordLess);
        coinbase.mpa = records;
        block.vtx[0] = MakeTransactionRef(coinbase);
        if (modern::BlockHasAnyMpa(block)) {
            const uint256 root{modern::ComputePayloadRoot(block)};
            CMutableTransaction cb2{*block.vtx[0]};
            cb2.vout.emplace_back(0, modern::MakePayloadRootCellScript(root));
            block.vtx[0] = MakeTransactionRef(cb2);
        }
        block.hashMerkleRoot = BlockMerkleRoot(block);
        Sign(block, m_validator_a);
        if (block.GetBlockTime() > GetTime()) SetMockTime(block.GetBlockTime());
        return block;
    }
    std::string ProbeFresh(const std::vector<std::pair<CScript, CMpaRecord>>& v1, const std::vector<CMpaRecord>& v2)
    {
        return Probe(BuildFreshBlock(v1, v2));
    }
    void ProduceFresh(const std::vector<std::pair<CScript, CMpaRecord>>& v1, const std::vector<CMpaRecord>& v2)
    {
        const int prev_height{Tip()->nHeight};
        const CBlock block{BuildFreshBlock(v1, v2)};
        BOOST_REQUIRE_MESSAGE(Probe(block).empty(), "block at " << prev_height + 1 << " invalid: " << Probe(block));
        BOOST_REQUIRE(Submit(block));
        BOOST_REQUIRE_EQUAL(Tip()->nHeight, prev_height + 1);
    }
    CBlockIndex* Lookup(const uint256& hash)
    {
        CBlockIndex* index{WITH_LOCK(cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(hash))};
        BOOST_REQUIRE(index != nullptr);
        return index;
    }
    void Invalidate(const uint256& hash)
    {
        BlockValidationState state;
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().InvalidateBlock(state, Lookup(hash)));
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().ActivateBestChain(state));
    }
    void Reconsider(const uint256& hash)
    {
        CBlockIndex* index{Lookup(hash)};
        {
            LOCK(cs_main);
            m_node.chainman->ActiveChainstate().ResetBlockFailureFlags(index);
            m_node.chainman->RecalculateBestHeader();
        }
        BlockValidationState state;
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().ActivateBestChain(state));
    }
    /**
     * The V1 reference: a tracker WITHOUT the fresh lineage (V1 exactly as on
     * every shipped network) fed the active chain block by block from M,
     * each block with its type-11 records stripped (only the fresh judge
     * reads them, and it would refuse them without the knob). Every V1 field
     * of the live key-mode tracker must equal it.
     */
    void CheckV1AgainstReference()
    {
        LOCK(cs_main);
        const CChain& chain{m_node.chainman->ActiveChain()};
        const node::BlockManager& blockman{m_node.chainman->m_blockman};
        Consensus::Params v1_only{m_node.chainman->GetConsensus()};
        v1_only.fresh_lineage.reset();
        node::FinalityTracker reference;
        BOOST_REQUIRE(reference.Sync(chain, blockman, v1_only, *chain[m_M - 1]));
        for (int h{m_M}; h <= chain.Height(); ++h) {
            CBlock block;
            BOOST_REQUIRE(blockman.ReadBlock(block, *chain[h]));
            CMutableTransaction coinbase{*block.vtx[0]};
            std::erase_if(coinbase.mpa, [](const CMpaRecord& rec) {
                return rec.payload_type == modern::MPA_TYPE_FINALITY_CERTIFICATE_V2;
            });
            block.vtx[0] = MakeTransactionRef(coinbase);
            reference.BlockConnected(block, *chain[h], v1_only);
            BOOST_REQUIRE_MESSAGE(reference.Synced(chain[h]->GetBlockHash()), "V1 reference stalled at " << h);
        }
        BOOST_CHECK(!reference.Current().fresh);
        BOOST_CHECK_MESSAGE(SameV1Fields(Finality().Current(), reference.Current()),
                            "V1 state differs from the reference at height " << chain.Height());
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(finality_fresh_key_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(codec_round_trips_and_exact_sizes)
{
    BOOST_CHECK_EQUAL(modern::CREATION_ACTION_FRESH_FINALITY_KEY_EVIDENCE, 12);
    BOOST_CHECK_EQUAL(T12, 12);
    BOOST_CHECK(!modern::IsKnownCreationAction(12, 1)); // an MPA record, never a CreationAction
    BOOST_CHECK_EQUAL(modern::FRESH_FINALITY_KEY_POLICY_VERSION, 2);
    BOOST_CHECK_EQUAL(FreshFinalityKeyParams::SIZE, 56U);
    BOOST_CHECK_EQUAL(FreshFinalityKeyEvidence::SIZE, 248U);
    BOOST_CHECK_EQUAL(modern::FRESH_FINALITY_KEY_EVIDENCE_SIZE, 248U);

    const Validator a{1};
    const auto sk{BlsKey(1)};
    const Built b{Build(a, sk, 0x0102030405060708ULL)};

    // Params: bls_pubkey || u64 BE target_epoch.
    const auto params_bytes{b.params.Encode()};
    BOOST_CHECK(std::equal(b.params.bls_pubkey.begin(), b.params.bls_pubkey.end(), params_bytes.begin()));
    for (size_t i = 0; i < 8; ++i) BOOST_CHECK_EQUAL(params_bytes[48 + i], i + 1);
    const auto params_rt{FreshFinalityKeyParams::Decode(params_bytes)};
    BOOST_REQUIRE(params_rt);
    BOOST_CHECK(*params_rt == b.params);
    for (const size_t bad : {size_t{0}, size_t{52}, size_t{55}, size_t{57}}) {
        const std::vector<unsigned char> bytes(bad, 0x01);
        BOOST_CHECK(!FreshFinalityKeyParams::Decode(bytes));
    }

    // Evidence: vk[32] || pk[48] || u64 BE epoch || bip340[64] || pop[96].
    const auto ev_bytes{b.ev.Encode()};
    BOOST_CHECK(std::equal(a.vk.begin(), a.vk.end(), ev_bytes.begin()));
    BOOST_CHECK(std::equal(b.ev.bls_pubkey.begin(), b.ev.bls_pubkey.end(), ev_bytes.begin() + 32));
    for (size_t i = 0; i < 8; ++i) BOOST_CHECK_EQUAL(ev_bytes[80 + i], i + 1);
    BOOST_CHECK(std::equal(b.ev.bip340_sig.begin(), b.ev.bip340_sig.end(), ev_bytes.begin() + 88));
    BOOST_CHECK(std::equal(b.ev.pop.begin(), b.ev.pop.end(), ev_bytes.begin() + 152));
    const auto ev_rt{FreshFinalityKeyEvidence::Decode(ev_bytes)};
    BOOST_REQUIRE(ev_rt);
    BOOST_CHECK(*ev_rt == b.ev);
    for (const size_t bad : {size_t{0}, size_t{244}, size_t{247}, size_t{249}}) {
        const std::vector<unsigned char> bytes(bad, 0x01);
        BOOST_CHECK(!FreshFinalityKeyEvidence::Decode(bytes));
    }

    // Cell: PUSH("B3MC" || 0x0007 || 0x0002 || vk || params) OP_DROP OP_FALSE, zero value.
    const CScript cell{modern::MakeFreshFinalityKeyCell(a.vk, b.params)};
    std::vector<unsigned char> payload{'B', '3', 'M', 'C', 0x00, 0x07, 0x00, 0x02};
    payload.insert(payload.end(), a.vk.begin(), a.vk.end());
    payload.insert(payload.end(), params_bytes.begin(), params_bytes.end());
    BOOST_CHECK_EQUAL(payload.size(), 96U);
    BOOST_CHECK(cell == (CScript() << payload << OP_DROP << OP_FALSE));
    const auto parsed{modern::ParseMetadataCell(cell)};
    BOOST_REQUIRE(parsed);
    BOOST_CHECK_EQUAL(parsed->policy_type, FINALITY_KEY_POLICY);
    BOOST_CHECK_EQUAL(parsed->policy_version, 2);
    BOOST_CHECK(parsed->commitment == a.Commitment());
    BOOST_CHECK(std::equal(params_bytes.begin(), params_bytes.end(), parsed->params.begin(), parsed->params.end()));
    BOOST_CHECK(modern::ParseFreshFinalityKeyCell(cell));
    {
        modern::FinalityKeyParams v1;
        v1.bls_pubkey = Pk(sk);
        const auto v1_cell{modern::MakeMetadataCellScript(FINALITY_KEY_POLICY, modern::POLICY_VERSION_V1,
                                                          a.Commitment(), v1.Encode())};
        BOOST_REQUIRE(v1_cell);
        BOOST_CHECK(!modern::ParseFreshFinalityKeyCell(*v1_cell));
    }

    // Record: type 12, version 1, the 248 evidence bytes.
    const CMpaRecord rec{modern::MakeFreshFinalityKeyRecord(b.ev)};
    BOOST_CHECK_EQUAL(rec.payload_type, 12);
    BOOST_CHECK_EQUAL(rec.payload_version, 1);
    BOOST_CHECK(std::equal(ev_bytes.begin(), ev_bytes.end(), rec.payload.begin(), rec.payload.end()));
}

BOOST_AUTO_TEST_CASE(fresh_bind_digest_layout_and_separation)
{
    const Validator a{1};
    const BlsPubkeyBytes pk{Pk(BlsKey(1))};
    const uint64_t epoch{0x0a0b0c0d0e0f1011ULL};

    // TaggedHash("B3/FINALITY/FRESHBIND/V1", domain[32] || vk[32] || pk[48] || u64 BE epoch).
    std::vector<unsigned char> pre(CHAIN_DOMAIN.begin(), CHAIN_DOMAIN.end());
    pre.insert(pre.end(), a.vk.begin(), a.vk.end());
    pre.insert(pre.end(), pk.begin(), pk.end());
    for (int i = 7; i >= 0; --i) pre.push_back(static_cast<unsigned char>(epoch >> (8 * i)));
    BOOST_CHECK_EQUAL(pre.size(), 120U);
    HashWriter manual{TaggedHash("B3/FINALITY/FRESHBIND/V1")};
    manual << std::span<const unsigned char>(pre);
    const uint256 digest{modern::FreshFinalityBindDigest(CHAIN_DOMAIN, a.vk, pk, epoch)};
    BOOST_CHECK(digest == manual.GetSHA256());
    BOOST_CHECK_EQUAL(std::string{modern::FINALITY_FRESH_BIND_TAG}, "B3/FINALITY/FRESHBIND/V1");

    // Every field is bound.
    const Validator b{2};
    BlsPubkeyBytes pk2{pk};
    pk2[47] ^= 0x01;
    BOOST_CHECK(digest != modern::FreshFinalityBindDigest(OTHER_CHAIN_DOMAIN, a.vk, pk, epoch));
    BOOST_CHECK(digest != modern::FreshFinalityBindDigest(CHAIN_DOMAIN, b.vk, pk, epoch));
    BOOST_CHECK(digest != modern::FreshFinalityBindDigest(CHAIN_DOMAIN, a.vk, pk2, epoch));
    BOOST_CHECK(digest != modern::FreshFinalityBindDigest(CHAIN_DOMAIN, a.vk, pk, epoch + 1));

    // Distinct from V1's bind digest for the same identity and key.
    for (const uint32_t seq : {0U, 1U, static_cast<uint32_t>(epoch)}) {
        BOOST_CHECK(modern::FreshFinalityBindDigest(CHAIN_DOMAIN, a.vk, pk, seq) !=
                    modern::FinalityBindDigest(CHAIN_DOMAIN, a.vk, pk, seq));
    }
}

BOOST_AUTO_TEST_CASE(check_ok_identity_domain_and_v1_replay)
{
    const Validator a{1}, mallory{9};
    const auto k1{BlsKey(1)};
    const Built ok{Build(a, k1, 3)};
    BOOST_CHECK(Check(a, ok) == FreshKeyCheck::OK);
    BOOST_CHECK(Check(a, Build(a, k1, 0)) == FreshKeyCheck::OK);
    BOOST_CHECK(Check(a, Build(a, k1, std::numeric_limits<uint64_t>::max())) == FreshKeyCheck::OK);

    // Signed by another identity key.
    BOOST_CHECK(Check(a, Build(a, k1, 3, CHAIN_DOMAIN, &mallory.identity)) == FreshKeyCheck::BAD_BIP340_SIGNATURE);
    // Signed under another chain domain.
    const Built other_domain{Build(a, k1, 3, OTHER_CHAIN_DOMAIN)};
    BOOST_CHECK(Check(a, other_domain) == FreshKeyCheck::BAD_BIP340_SIGNATURE);
    BOOST_CHECK(Check(a, other_domain, OTHER_CHAIN_DOMAIN) == FreshKeyCheck::OK);
    // Tampered signature.
    Built tampered{ok};
    tampered.ev.bip340_sig[17] ^= 0x01;
    BOOST_CHECK(Check(a, tampered) == FreshKeyCheck::BAD_BIP340_SIGNATURE);
    // A signature for epoch 3 re-labelled as epoch 4 in both cell and evidence.
    Built relabelled{ok};
    relabelled.params.target_epoch = 4;
    relabelled.ev.target_epoch = 4;
    BOOST_CHECK(Check(a, relabelled) == FreshKeyCheck::BAD_BIP340_SIGNATURE);

    // V1-tag replay: a V1 FINALITY_KEY bind signature (seq 0 or seq = epoch)
    // never authorizes a fresh binding of the same key.
    for (const uint32_t seq : {0U, 3U}) {
        Built replay{ok};
        const uint256 v1_digest{modern::FinalityBindDigest(CHAIN_DOMAIN, a.vk, replay.ev.bls_pubkey, seq)};
        uint256 aux{};
        BOOST_REQUIRE(a.identity.SignSchnorr(v1_digest, replay.ev.bip340_sig, nullptr, aux));
        BOOST_CHECK(Check(a, replay) == FreshKeyCheck::BAD_BIP340_SIGNATURE);
    }
    // ... and a fresh signature never authorizes a V1 binding.
    {
        modern::FinalityKeyParams v1p;
        v1p.bls_pubkey = ok.params.bls_pubkey;
        v1p.seq = 3;
        modern::FinalityKeyEvidence v1e;
        v1e.validator_key = a.vk;
        v1e.bls_pubkey = v1p.bls_pubkey;
        v1e.seq = 3;
        v1e.bip340_sig = ok.ev.bip340_sig;
        v1e.pop = ok.ev.pop;
        const modern::BlsKeyOwnerLookup none{[](const BlsPubkeyBytes&) { return std::optional<ValidatorKeyBytes>{}; }};
        const modern::BindingRecord prev{BlsPubkeyBytes{}, 2, 1};
        BOOST_CHECK(modern::CheckFinalityKeyTransition(CHAIN_DOMAIN, a.Commitment(), v1p, v1e, prev, none) ==
                    modern::FinalityKeyCheck::BAD_BIP340_SIGNATURE);
    }
}

BOOST_AUTO_TEST_CASE(check_fields_zero_key_bls_key_and_pop)
{
    const Validator a{1}, b{2};
    const auto k1{BlsKey(1)}, k2{BlsKey(2)};
    const Built ok{Build(a, k1, 5)};

    // Evidence <-> cell field mismatches, checked before any cryptography.
    BOOST_CHECK(Check(b, ok) == FreshKeyCheck::EVIDENCE_VALIDATOR_MISMATCH);
    Built pk_mismatch{ok};
    pk_mismatch.params.bls_pubkey = Pk(k2);
    BOOST_CHECK(Check(a, pk_mismatch) == FreshKeyCheck::EVIDENCE_PUBKEY_MISMATCH);
    Built epoch_mismatch{ok};
    epoch_mismatch.params.target_epoch = 6;
    BOOST_CHECK(Check(a, epoch_mismatch) == FreshKeyCheck::EVIDENCE_EPOCH_MISMATCH);

    // The all-zero key has no meaning in the fresh kind (no revocation), even
    // with a valid identity signature over it.
    Built zero{ok};
    zero.params.bls_pubkey.fill(0);
    zero.ev.bls_pubkey.fill(0);
    zero.ev.pop.fill(0);
    SignIdentity(zero, CHAIN_DOMAIN, a.identity);
    BOOST_CHECK(Check(a, zero) == FreshKeyCheck::ZERO_KEY);

    // Non-canonical key bytes (compression flag cleared): checked before PoP.
    Built bad_key{ok};
    bad_key.params.bls_pubkey[0] = 0x11;
    bad_key.ev.bls_pubkey = bad_key.params.bls_pubkey;
    SignIdentity(bad_key, CHAIN_DOMAIN, a.identity);
    BOOST_CHECK(Check(a, bad_key) == FreshKeyCheck::BAD_BLS_PUBKEY);

    // Proof of possession.
    Built cross{ok};
    cross.ev.pop = k2.SignPoP().Compressed();
    BOOST_CHECK(Check(a, cross) == FreshKeyCheck::BAD_POP);
    Built tampered{ok};
    tampered.ev.pop[5] ^= 0x01;
    BOOST_CHECK(Check(a, tampered) == FreshKeyCheck::BAD_POP);
    Built missing{ok};
    missing.ev.pop.fill(0);
    BOOST_CHECK(Check(a, missing) == FreshKeyCheck::BAD_POP);
    Built sig_not_pop{ok};
    std::array<unsigned char, 32> d{};
    std::copy(ok.params.bls_pubkey.begin(), ok.params.bls_pubkey.begin() + 32, d.begin());
    sig_not_pop.ev.pop = k1.Sign(d).Compressed();
    BOOST_CHECK(Check(a, sig_not_pop) == FreshKeyCheck::BAD_POP);
    // The bare IETF PoP (sign(sk, pk) under POP_DST, V1's suite) is not the
    // fresh kind's proof: it names no validator, chain or epoch.
    Built ietf_pop{ok};
    ietf_pop.ev.pop = k1.SignPoP().Compressed();
    BOOST_CHECK(Check(a, ietf_pop) == FreshKeyCheck::BAD_POP);
    // The BIP340 digest signed by the BLS key is not it either (own tag).
    Built bind_digest{ok};
    {
        const uint256 bd{modern::FreshFinalityBindDigest(CHAIN_DOMAIN, a.vk, ok.ev.bls_pubkey, 5)};
        bind_digest.ev.pop = k1.Sign(std::span<const unsigned char>(bd.begin(), 32)).Compressed();
    }
    BOOST_CHECK(Check(a, bind_digest) == FreshKeyCheck::BAD_POP);
    // Each bound field: a proof for another epoch, chain or validator fails
    // even with a valid identity signature over the claim.
    Built other_epoch{ok};
    other_epoch.ev.pop = FreshPop(k1, CHAIN_DOMAIN, a.vk, ok.ev.bls_pubkey, 6);
    BOOST_CHECK(Check(a, other_epoch) == FreshKeyCheck::BAD_POP);
    Built other_chain{ok};
    other_chain.ev.pop = FreshPop(k1, OTHER_CHAIN_DOMAIN, a.vk, ok.ev.bls_pubkey, 5);
    BOOST_CHECK(Check(a, other_chain) == FreshKeyCheck::BAD_POP);
    Built other_validator{ok};
    other_validator.ev.pop = FreshPop(k1, CHAIN_DOMAIN, b.vk, ok.ev.bls_pubkey, 5);
    BOOST_CHECK(Check(a, other_validator) == FreshKeyCheck::BAD_POP);

    // Names used by the consensus error strings ("fresh-key-<name>").
    BOOST_CHECK_EQUAL(std::string{modern::FreshKeyCheckName(FreshKeyCheck::OK)}, "ok");
    BOOST_CHECK_EQUAL(std::string{modern::FreshKeyCheckName(FreshKeyCheck::EVIDENCE_VALIDATOR_MISMATCH)}, "evidence-validator-mismatch");
    BOOST_CHECK_EQUAL(std::string{modern::FreshKeyCheckName(FreshKeyCheck::EVIDENCE_PUBKEY_MISMATCH)}, "evidence-pubkey-mismatch");
    BOOST_CHECK_EQUAL(std::string{modern::FreshKeyCheckName(FreshKeyCheck::EVIDENCE_EPOCH_MISMATCH)}, "evidence-epoch-mismatch");
    BOOST_CHECK_EQUAL(std::string{modern::FreshKeyCheckName(FreshKeyCheck::ZERO_KEY)}, "zero-key");
    BOOST_CHECK_EQUAL(std::string{modern::FreshKeyCheckName(FreshKeyCheck::BAD_BIP340_SIGNATURE)}, "bad-bip340-signature");
    BOOST_CHECK_EQUAL(std::string{modern::FreshKeyCheckName(FreshKeyCheck::BAD_BLS_PUBKEY)}, "bad-bls-pubkey");
    BOOST_CHECK_EQUAL(std::string{modern::FreshKeyCheckName(FreshKeyCheck::BAD_POP)}, "bad-pop");
}

BOOST_AUTO_TEST_CASE(pop_digest_layout_and_separation)
{
    const Validator a{1}, b{2};
    const BlsPubkeyBytes pk{Pk(BlsKey(1))};
    const uint64_t epoch{0x0a0b0c0d0e0f1011ULL};
    // TaggedHash("B3/FINALITY/FRESHPOP/V1", domain[32] || vk[32] || pk[48] || u64 BE epoch).
    std::vector<unsigned char> pre(CHAIN_DOMAIN.begin(), CHAIN_DOMAIN.end());
    pre.insert(pre.end(), a.vk.begin(), a.vk.end());
    pre.insert(pre.end(), pk.begin(), pk.end());
    for (int i = 7; i >= 0; --i) pre.push_back(static_cast<unsigned char>(epoch >> (8 * i)));
    HashWriter manual{TaggedHash("B3/FINALITY/FRESHPOP/V1")};
    manual << std::span<const unsigned char>(pre);
    const uint256 digest{modern::FreshFinalityPopDigest(CHAIN_DOMAIN, a.vk, pk, epoch)};
    BOOST_CHECK(digest == manual.GetSHA256());
    BOOST_CHECK_EQUAL(std::string{modern::FINALITY_FRESH_POP_TAG}, "B3/FINALITY/FRESHPOP/V1");
    // Its own tag: never the BIP340 bind digest, and every field is bound.
    BOOST_CHECK(digest != modern::FreshFinalityBindDigest(CHAIN_DOMAIN, a.vk, pk, epoch));
    BlsPubkeyBytes pk2{pk};
    pk2[47] ^= 0x01;
    BOOST_CHECK(digest != modern::FreshFinalityPopDigest(OTHER_CHAIN_DOMAIN, a.vk, pk, epoch));
    BOOST_CHECK(digest != modern::FreshFinalityPopDigest(CHAIN_DOMAIN, b.vk, pk, epoch));
    BOOST_CHECK(digest != modern::FreshFinalityPopDigest(CHAIN_DOMAIN, a.vk, pk2, epoch));
    BOOST_CHECK(digest != modern::FreshFinalityPopDigest(CHAIN_DOMAIN, a.vk, pk, epoch + 1));
}

BOOST_AUTO_TEST_CASE(front_running_copies_of_a_pending_binding_are_refused)
{
    // Codex review finding: with a bare IETF PoP in the evidence, anyone
    // could copy a pending binding's key and PoP into a claim of their own
    // and, mined first, burn the key (first claim wins) so the member sits
    // out. The identity-bound proof makes every such copy invalid.
    const Validator honest{1}, mallory{9};
    const auto k{BlsKey(1)};
    const Built ok{Build(honest, k, 4)};
    BOOST_REQUIRE(Check(honest, ok) == FreshKeyCheck::OK);

    // (a) A fresh copy under mallory's identity, with mallory's own valid
    //     BIP340 authorization of the same key and epoch.
    Built copy{ok};
    copy.ev.validator_key = mallory.vk;
    SignIdentity(copy, CHAIN_DOMAIN, mallory.identity);
    BOOST_CHECK(Check(mallory, copy) == FreshKeyCheck::BAD_POP);
    // ... for any epoch.
    for (const uint64_t e : {uint64_t{0}, uint64_t{2}, uint64_t{4}, uint64_t{5}}) {
        Built relabelled{copy};
        relabelled.params.target_epoch = e;
        relabelled.ev.target_epoch = e;
        SignIdentity(relabelled, CHAIN_DOMAIN, mallory.identity);
        BOOST_CHECK(Check(mallory, relabelled) == FreshKeyCheck::BAD_POP);
    }
    // (b) A V1 FINALITY_KEY copy under mallory's identity: V1 demands the
    //     bare IETF PoP of the key, which no fresh binding publishes; the
    //     fresh proof does not pass for it.
    {
        modern::FinalityKeyParams v1p;
        v1p.bls_pubkey = ok.params.bls_pubkey;
        v1p.seq = 0;
        modern::FinalityKeyEvidence v1e;
        v1e.validator_key = mallory.vk;
        v1e.bls_pubkey = v1p.bls_pubkey;
        v1e.seq = 0;
        const uint256 v1_digest{modern::FinalityBindDigest(CHAIN_DOMAIN, mallory.vk, v1e.bls_pubkey, 0)};
        uint256 aux{};
        BOOST_REQUIRE(mallory.identity.SignSchnorr(v1_digest, v1e.bip340_sig, nullptr, aux));
        v1e.pop = ok.ev.pop;
        const modern::BlsKeyOwnerLookup none{[](const BlsPubkeyBytes&) { return std::optional<ValidatorKeyBytes>{}; }};
        BOOST_CHECK(modern::CheckFinalityKeyTransition(CHAIN_DOMAIN, mallory.Commitment(), v1p, v1e, std::nullopt,
                                                       none) == modern::FinalityKeyCheck::BAD_POP);
        // Only the key's holder could produce the V1 proof.
        v1e.pop = k.SignPoP().Compressed();
        BOOST_CHECK(modern::CheckFinalityKeyTransition(CHAIN_DOMAIN, mallory.Commitment(), v1p, v1e, std::nullopt,
                                                       none) == modern::FinalityKeyCheck::OK);
    }
    // (c) The honest binding's proof is not reusable on another chain.
    BOOST_CHECK(Check(honest, ok, OTHER_CHAIN_DOMAIN) == FreshKeyCheck::BAD_BIP340_SIGNATURE);
    Built cross_chain{Build(honest, k, 4, OTHER_CHAIN_DOMAIN)};
    cross_chain.ev.pop = ok.ev.pop;
    BOOST_CHECK(Check(honest, cross_chain, OTHER_CHAIN_DOMAIN) == FreshKeyCheck::BAD_POP);
}

BOOST_AUTO_TEST_CASE(matcher_bijection)
{
    const Validator a{1}, b{2};
    const auto k1{BlsKey(1)}, k2{BlsKey(2)}, k3{BlsKey(3)};
    std::vector<FreshFinalityKeyPair> pairs;
    std::string err;

    // No fresh objects: trivially fine.
    BOOST_CHECK(MatchFresh(BaseTx(), pairs, err));
    BOOST_CHECK(pairs.empty());

    // One pair: indices point at the cell and the record.
    {
        CMutableTransaction tx{BaseTx()};
        const Built p{Build(a, k1, 0)};
        AddPair(tx, a, p);
        BOOST_REQUIRE(MatchFresh(tx, pairs, err));
        BOOST_REQUIRE_EQUAL(pairs.size(), 1U);
        BOOST_CHECK_EQUAL(pairs[0].cell_index, 1U);
        BOOST_CHECK_EQUAL(pairs[0].record_index, 0U);
        BOOST_CHECK(pairs[0].commitment == a.Commitment());
        BOOST_CHECK(pairs[0].params == p.params);
        BOOST_CHECK(pairs[0].evidence == p.ev);
    }
    // Several pairs, including one validator naming two epochs and two keys
    // for one epoch (seat rules, not the matcher, judge those): key order.
    {
        CMutableTransaction tx{BaseTx()};
        AddPair(tx, b, Build(b, k3, 2));
        AddPair(tx, a, Build(a, k1, 2));
        AddPair(tx, a, Build(a, k2, 2));
        AddPair(tx, a, Build(a, k2, 3));
        BOOST_REQUIRE(MatchFresh(tx, pairs, err));
        BOOST_REQUIRE_EQUAL(pairs.size(), 4U);
        for (size_t i = 1; i < pairs.size(); ++i) {
            BOOST_CHECK(modern::MakeFreshFinalityKeyMatchKey(pairs[i - 1].evidence.validator_key, pairs[i - 1].params) <
                        modern::MakeFreshFinalityKeyMatchKey(pairs[i].evidence.validator_key, pairs[i].params));
        }
        for (const auto& pair : pairs) {
            BOOST_CHECK(tx.vout[pair.cell_index] == FreshCell(pair.commitment == a.Commitment() ? a : b, pair.params));
            BOOST_CHECK(tx.mpa[pair.record_index] == modern::MakeFreshFinalityKeyRecord(pair.evidence));
        }
    }
    // Duplicate cell.
    {
        CMutableTransaction tx{BaseTx()};
        const Built p{Build(a, k1, 0)};
        AddPair(tx, a, p);
        tx.vout.push_back(FreshCell(a, p.params));
        BOOST_CHECK(!MatchFresh(tx, pairs, err));
        BOOST_CHECK_EQUAL(err, "fresh-key-duplicate-cell");
    }
    // Duplicate evidence.
    {
        CMutableTransaction tx{BaseTx()};
        const Built p{Build(a, k1, 0)};
        AddPair(tx, a, p);
        tx.mpa.push_back(modern::MakeFreshFinalityKeyRecord(p.ev));
        BOOST_CHECK(!MatchFresh(tx, pairs, err));
        BOOST_CHECK_EQUAL(err, "fresh-key-duplicate-evidence");
    }
    // Orphan evidence (no cell), and a field mismatch, which is an orphan too.
    {
        CMutableTransaction tx{BaseTx()};
        tx.mpa.push_back(modern::MakeFreshFinalityKeyRecord(Build(a, k1, 0).ev));
        BOOST_CHECK(!MatchFresh(tx, pairs, err));
        BOOST_CHECK_EQUAL(err, "fresh-key-orphan-evidence");
        CMutableTransaction mismatch{BaseTx()};
        const Built p{Build(a, k1, 0)};
        mismatch.vout.push_back(FreshCell(a, p.params));
        Built other_epoch{p};
        other_epoch.ev.target_epoch = 1;
        mismatch.mpa.push_back(modern::MakeFreshFinalityKeyRecord(other_epoch.ev));
        BOOST_CHECK(!MatchFresh(mismatch, pairs, err));
        BOOST_CHECK_EQUAL(err, "fresh-key-orphan-evidence");
    }
    // Cell without evidence.
    {
        CMutableTransaction tx{BaseTx()};
        AddPair(tx, a, Build(a, k1, 0));
        tx.vout.push_back(FreshCell(a, Build(a, k2, 0).params));
        BOOST_CHECK(!MatchFresh(tx, pairs, err));
        BOOST_CHECK_EQUAL(err, "fresh-key-cell-without-evidence");
    }
    // Malformed cell params under policy version 2 (V1's 52 bytes included).
    for (const size_t len : {size_t{55}, size_t{57}, size_t{52}}) {
        CMutableTransaction tx{BaseTx()};
        const std::vector<unsigned char> params(len, 0x42);
        const auto cell{modern::MakeMetadataCellScript(FINALITY_KEY_POLICY, modern::FRESH_FINALITY_KEY_POLICY_VERSION,
                                                       a.Commitment(), params)};
        BOOST_REQUIRE(cell);
        tx.vout.emplace_back(0, *cell);
        BOOST_CHECK(!MatchFresh(tx, pairs, err));
        BOOST_CHECK_EQUAL(err, "fresh-key-cell-params");
    }
    // Malformed evidence (wrong size).
    for (const size_t len : {size_t{244}, size_t{247}, size_t{249}}) {
        CMutableTransaction tx{BaseTx()};
        CMpaRecord rec{modern::MakeFreshFinalityKeyRecord(Build(a, k1, 0).ev)};
        rec.payload.resize(len, 0x00);
        tx.mpa.push_back(rec);
        BOOST_CHECK(!MatchFresh(tx, pairs, err));
        BOOST_CHECK_EQUAL(err, "fresh-key-evidence-malformed");
    }
    // A type-12 record of another version is not this matcher's object (the
    // registry rejects it as unknown before any matcher runs).
    {
        CMutableTransaction tx{BaseTx()};
        CMpaRecord rec{modern::MakeFreshFinalityKeyRecord(Build(a, k1, 0).ev)};
        rec.payload_version = 2;
        tx.mpa.push_back(rec);
        BOOST_CHECK(MatchFresh(tx, pairs, err));
        BOOST_CHECK(pairs.empty());
    }
}

BOOST_AUTO_TEST_CASE(v1_and_fresh_matchers_ignore_each_other)
{
    const Validator a{1}, b{2};
    const auto k1{BlsKey(1)}, k2{BlsKey(2)};
    std::vector<FreshFinalityKeyPair> fresh_pairs;
    std::vector<modern::FinalityKeyPair> v1_pairs;
    std::string err;

    // One V1 pair and one fresh pair side by side: each matcher sees its own.
    {
        CMutableTransaction tx{BaseTx()};
        AddV1Pair(tx, a, k1);
        AddPair(tx, b, Build(b, k2, 0));
        BOOST_REQUIRE(modern::MatchFinalityKeyPairs(CTransaction{tx}, v1_pairs, err));
        BOOST_REQUIRE_EQUAL(v1_pairs.size(), 1U);
        BOOST_CHECK(v1_pairs[0].evidence.validator_key == a.vk);
        BOOST_REQUIRE(MatchFresh(tx, fresh_pairs, err));
        BOOST_REQUIRE_EQUAL(fresh_pairs.size(), 1U);
        BOOST_CHECK(fresh_pairs[0].evidence.validator_key == b.vk);
    }
    // Fresh objects only: the V1 matcher skips policy-version-2 cells (it
    // used to fail them as finality-key-cell-version) and type-12 records.
    {
        CMutableTransaction tx{BaseTx()};
        AddPair(tx, a, Build(a, k1, 0));
        BOOST_CHECK(modern::MatchFinalityKeyPairs(CTransaction{tx}, v1_pairs, err));
        BOOST_CHECK(v1_pairs.empty());
    }
    // V1 objects only: the fresh matcher sees nothing.
    {
        CMutableTransaction tx{BaseTx()};
        AddV1Pair(tx, a, k1);
        BOOST_CHECK(MatchFresh(tx, fresh_pairs, err));
        BOOST_CHECK(fresh_pairs.empty());
    }
    // Other FINALITY_KEY policy versions keep failing the V1 matcher.
    for (const uint16_t version : {uint16_t{0}, uint16_t{3}}) {
        CMutableTransaction tx{BaseTx()};
        const auto cell{modern::MakeMetadataCellScript(FINALITY_KEY_POLICY, version, a.Commitment(),
                                                       Build(a, k1, 0).params.Encode())};
        BOOST_REQUIRE(cell);
        tx.vout.emplace_back(0, *cell);
        BOOST_CHECK(!modern::MatchFinalityKeyPairs(CTransaction{tx}, v1_pairs, err));
        BOOST_CHECK_EQUAL(err, "finality-key-cell-version");
        BOOST_CHECK(MatchFresh(tx, fresh_pairs, err));
        BOOST_CHECK(fresh_pairs.empty());
    }
    // A fresh cell never satisfies a type-5 record, nor a V1 cell a type-12 record.
    {
        CMutableTransaction tx{BaseTx()};
        AddV1Pair(tx, a, k1);
        tx.vout.erase(tx.vout.begin() + 1); // drop the V1 cell, keep its type-5 record
        tx.vout.push_back(FreshCell(a, Build(a, k1, 0).params));
        BOOST_CHECK(!modern::MatchFinalityKeyPairs(CTransaction{tx}, v1_pairs, err));
        BOOST_CHECK_EQUAL(err, "finality-key-orphan-evidence");
        BOOST_CHECK(!MatchFresh(tx, fresh_pairs, err));
        BOOST_CHECK_EQUAL(err, "fresh-key-cell-without-evidence");
    }
    {
        CMutableTransaction tx{BaseTx()};
        AddV1Pair(tx, a, k1);
        tx.mpa.clear(); // keep the V1 cell, replace its evidence with a type-12 record
        tx.mpa.push_back(modern::MakeFreshFinalityKeyRecord(Build(a, k1, 0).ev));
        BOOST_CHECK(!modern::MatchFinalityKeyPairs(CTransaction{tx}, v1_pairs, err));
        BOOST_CHECK_EQUAL(err, "finality-key-cell-without-evidence");
        BOOST_CHECK(!MatchFresh(tx, fresh_pairs, err));
        BOOST_CHECK_EQUAL(err, "fresh-key-orphan-evidence");
    }
}

BOOST_AUTO_TEST_CASE(verify_bindings_in_block)
{
    const Validator a{1}, b{2};
    const auto k1{BlsKey(1)}, k2{BlsKey(2)};
    std::string err;
    CBlock block;
    {
        CMutableTransaction coinbase;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vout.emplace_back(0, CScript() << OP_TRUE);
        block.vtx.push_back(MakeTransactionRef(coinbase));
    }
    BOOST_CHECK(modern::VerifyFreshKeyBindingsInBlock(block, CHAIN_DOMAIN, err));

    CMutableTransaction bindings{BaseTx()};
    AddPair(bindings, a, Build(a, k1, 0));
    AddPair(bindings, b, Build(b, k2, 0));
    CMutableTransaction v1_only{BaseTx()};
    v1_only.vin[0].prevout.n = 9;
    AddV1Pair(v1_only, a, BlsKey(7));
    CBlock good{block};
    good.vtx.push_back(MakeTransactionRef(bindings));
    good.vtx.push_back(MakeTransactionRef(v1_only));
    BOOST_CHECK(modern::VerifyFreshKeyBindingsInBlock(good, CHAIN_DOMAIN, err));
    // The chain domain is bound.
    BOOST_CHECK(!modern::VerifyFreshKeyBindingsInBlock(good, OTHER_CHAIN_DOMAIN, err));
    BOOST_CHECK_EQUAL(err, "fresh-key-bad-bip340-signature");

    // A bad proof of possession in a later transaction fails the block.
    {
        CMutableTransaction bad{BaseTx()};
        bad.vin[0].prevout.n = 5;
        Built p{Build(b, k1, 1)};
        p.ev.pop[3] ^= 0x01;
        AddPair(bad, b, p);
        CBlock block_bad{good};
        block_bad.vtx.push_back(MakeTransactionRef(bad));
        BOOST_CHECK(!modern::VerifyFreshKeyBindingsInBlock(block_bad, CHAIN_DOMAIN, err));
        BOOST_CHECK_EQUAL(err, "fresh-key-bad-pop");
    }
    // A zero key fails with its own name; a broken bijection with the matcher's.
    {
        CMutableTransaction zero{BaseTx()};
        Built p{Build(a, k1, 4)};
        p.params.bls_pubkey.fill(0);
        p.ev.bls_pubkey.fill(0);
        SignIdentity(p, CHAIN_DOMAIN, a.identity);
        AddPair(zero, a, p);
        CBlock block_zero{block};
        block_zero.vtx.push_back(MakeTransactionRef(zero));
        BOOST_CHECK(!modern::VerifyFreshKeyBindingsInBlock(block_zero, CHAIN_DOMAIN, err));
        BOOST_CHECK_EQUAL(err, "fresh-key-zero-key");

        CMutableTransaction orphan{BaseTx()};
        orphan.mpa.push_back(modern::MakeFreshFinalityKeyRecord(Build(a, k1, 0).ev));
        CBlock block_orphan{block};
        block_orphan.vtx.push_back(MakeTransactionRef(orphan));
        BOOST_CHECK(!modern::VerifyFreshKeyBindingsInBlock(block_orphan, CHAIN_DOMAIN, err));
        BOOST_CHECK_EQUAL(err, "fresh-key-orphan-evidence");
    }
}

BOOST_AUTO_TEST_CASE(registry_activation_size_and_cost)
{
    const Configs c;
    constexpr int R_KEY{Configs::R_KEY};
    constexpr int R_F{Configs::R_F};
    const auto status{[](const uint16_t version, const Consensus::Params& p, const std::optional<int> h) {
        return modern::GetPayloadTypeStatus(T12, version, p, h);
    }};
    const auto cell_active{[](const uint16_t version, const Consensus::Params& p, const std::optional<int> h) {
        return modern::IsMetadataCellActive(FINALITY_KEY_POLICY, version, p, h);
    }};

    // Without a key height (or without the X-pin configuration) type 12 and
    // the (7, 2) cell are inactive at every height; they never become UNKNOWN.
    for (const int h : {0, R_KEY - 1, R_KEY, R_F - 1, R_F, 1'000'000, std::numeric_limits<int>::max()}) {
        for (const Consensus::Params* p : {&c.production, &c.xpin, &c.fresh, &c.keyed_no_xpin}) {
            BOOST_CHECK(status(1, *p, h) == PayloadTypeStatus::INACTIVE);
            BOOST_CHECK(!cell_active(2, *p, h));
        }
        BOOST_CHECK(status(2, c.keyed, h) == PayloadTypeStatus::UNKNOWN);
        BOOST_CHECK(status(0, c.keyed, h) == PayloadTypeStatus::UNKNOWN);
        BOOST_CHECK(modern::GetPayloadTypeStatus(13, 1, c.keyed, h) == PayloadTypeStatus::UNKNOWN);
        // V1 FINALITY_KEY (7, 1) and type 5 keep the X-pin gate, untouched by R_key.
        BOOST_CHECK(cell_active(1, c.keyed, h) && cell_active(1, c.xpin, h));
        BOOST_CHECK(!cell_active(1, c.production, h));
        BOOST_CHECK(modern::GetPayloadTypeStatus(5, 1, c.keyed, h) == PayloadTypeStatus::ACTIVE);
        // Other versions and other metadata policies at version 2 stay inactive.
        BOOST_CHECK(!cell_active(3, c.keyed, h));
        BOOST_CHECK(!cell_active(0, c.keyed, h));
        for (const auto policy : {modern::PolicyType::FINALITY_CERT, modern::PolicyType::MODERN_PAYLOAD_ROOT,
                                  modern::PolicyType::BRIDGE_RECORD}) {
            BOOST_CHECK(!modern::IsMetadataCellActive(static_cast<uint16_t>(policy), 2, c.keyed, h));
        }
    }
    // With R_key: inactive below it, active from it; no height fails closed.
    BOOST_CHECK(status(1, c.keyed, 0) == PayloadTypeStatus::INACTIVE);
    BOOST_CHECK(status(1, c.keyed, R_KEY - 1) == PayloadTypeStatus::INACTIVE);
    BOOST_CHECK(status(1, c.keyed, std::nullopt) == PayloadTypeStatus::INACTIVE);
    BOOST_CHECK(!cell_active(2, c.keyed, 0));
    BOOST_CHECK(!cell_active(2, c.keyed, R_KEY - 1));
    BOOST_CHECK(!cell_active(2, c.keyed, std::nullopt));
    for (const int h : {R_KEY, R_KEY + 1, R_F - 1, R_F, 1'000'000, std::numeric_limits<int>::max()}) {
        BOOST_CHECK(status(1, c.keyed, h) == PayloadTypeStatus::ACTIVE);
        BOOST_CHECK(cell_active(2, c.keyed, h));
    }
    // Type 11 is unaffected by the key height.
    BOOST_CHECK(modern::GetPayloadTypeStatus(11, 1, c.keyed, R_F - 1) == PayloadTypeStatus::INACTIVE);
    BOOST_CHECK(modern::GetPayloadTypeStatus(11, 1, c.keyed, R_F) == PayloadTypeStatus::ACTIVE);

    // Size: exactly 248 bytes.
    BOOST_CHECK(modern::PayloadSizeAllowed(T12, 248));
    for (const size_t bad : {size_t{0}, size_t{244}, size_t{247}, size_t{249}, size_t{4000}}) {
        BOOST_CHECK(!modern::PayloadSizeAllowed(T12, bad));
    }

    // Cost: 700 per record (one BIP340 + one PoP, as type 5).
    BOOST_CHECK_EQUAL(modern::FRESH_FINALITY_KEY_EVIDENCE_VERIFY_COST, 700);
    BOOST_CHECK_EQUAL(modern::PayloadRecordVerifyCost(T12, 1), 700);
    BOOST_CHECK_EQUAL(modern::PayloadRecordVerifyCost(T12, 2), 0);
    BOOST_CHECK_EQUAL(modern::PayloadRecordVerifyCost(5, 1), 700);
    BOOST_CHECK_EQUAL(modern::PayloadRecordVerifyCost(11, 1), 2000);

    // Whole-transaction carrier and MPA rules on a real binding transaction.
    const Validator a{1};
    const auto k1{BlsKey(1)};
    CMutableTransaction tx{BaseTx()};
    AddPair(tx, a, Build(a, k1, 0));
    const CTransaction ctx{tx};
    std::string err;
    BOOST_CHECK_EQUAL(modern::PayloadVerifyCost(ctx), 700);
    BOOST_CHECK(modern::CheckMetadataCellOutputs(ctx, c.keyed, R_KEY, err));
    BOOST_CHECK(modern::CheckTransactionMpa(ctx, c.keyed, R_KEY, err));
    BOOST_CHECK(!modern::CheckMetadataCellOutputs(ctx, c.keyed, R_KEY - 1, err));
    BOOST_CHECK_EQUAL(err, "inactive metadata policy 7 v2 at output 1");
    BOOST_CHECK(!modern::CheckTransactionMpa(ctx, c.keyed, R_KEY - 1, err));
    BOOST_CHECK_EQUAL(err, "mpa-inactive-type");
    BOOST_CHECK(!modern::CheckTransactionMpa(ctx, c.keyed, err)); // context-free caller
    BOOST_CHECK_EQUAL(err, "mpa-inactive-type");
    for (const Consensus::Params* p : {&c.xpin, &c.fresh}) {
        BOOST_CHECK(!modern::CheckMetadataCellOutputs(ctx, *p, R_F, err));
        BOOST_CHECK(err.find("inactive metadata policy 7 v2") != std::string::npos);
        BOOST_CHECK(!modern::CheckTransactionMpa(ctx, *p, R_F, err));
        BOOST_CHECK_EQUAL(err, "mpa-inactive-type"); // was mpa-unknown-type before B-37
    }
    BOOST_CHECK(!modern::CheckTransactionMpa(ctx, c.production, R_F, err));
    BOOST_CHECK_EQUAL(err, "mpa-not-active");
    {
        CMutableTransaction resized{tx};
        resized.mpa[0].payload.resize(247);
        BOOST_CHECK(!modern::CheckTransactionMpa(CTransaction{resized}, c.keyed, R_KEY, err));
        BOOST_CHECK_EQUAL(err, "mpa-bad-record-size");
        CMutableTransaction versioned{tx};
        versioned.mpa[0].payload_version = 2;
        BOOST_CHECK(!modern::CheckTransactionMpa(CTransaction{versioned}, c.keyed, R_KEY, err));
        BOOST_CHECK_EQUAL(err, "mpa-unknown-type");
    }
    // The per-transaction budget admits 17 bindings (11,900) and refuses 18.
    {
        CMutableTransaction many{BaseTx()};
        for (unsigned i = 0; i < 18; ++i) {
            const Validator v{static_cast<unsigned char>(10 + i)};
            AddPair(many, v, Build(v, BlsKey(20 + i), 0));
            if (i == 16) {
                BOOST_CHECK_EQUAL(modern::PayloadVerifyCost(CTransaction{many}), 17 * 700);
                BOOST_CHECK(modern::CheckTransactionMpa(CTransaction{many}, c.keyed, R_KEY, err));
            }
        }
        BOOST_CHECK(!modern::CheckTransactionMpa(CTransaction{many}, c.keyed, R_KEY, err));
        BOOST_CHECK_EQUAL(err, "bad-payload-cost");
    }
}

BOOST_AUTO_TEST_CASE(shipped_networks_and_regtest_knob)
{
    // No shipped network configures a fresh lineage, so type 12 and the
    // (7, 2) cell are inactive at every height, including on mainnet where
    // the X-pin types 4/5 and the (7, 1) cell are live.
    for (const ChainType chain : {ChainType::MAIN, ChainType::TESTNET, ChainType::TESTNET4,
                                  ChainType::SIGNET, ChainType::REGTEST}) {
        const auto params{CreateChainParams(ArgsManager{}, chain)};
        const Consensus::Params& c{params->GetConsensus()};
        BOOST_CHECK(!c.fresh_lineage);
        for (const int h : {0, 241, 810'001, 820'001, 10'000'000, std::numeric_limits<int>::max()}) {
            BOOST_CHECK(!c.FreshKeyRulesActive(h));
            BOOST_CHECK(modern::GetPayloadTypeStatus(T12, 1, c, h) == PayloadTypeStatus::INACTIVE);
            BOOST_CHECK(!modern::IsMetadataCellActive(FINALITY_KEY_POLICY, 2, c, h));
        }
    }
    BOOST_CHECK(modern::IsMetadataCellActive(FINALITY_KEY_POLICY, 1,
                                             CreateChainParams(ArgsManager{}, ChainType::MAIN)->GetConsensus()));

    // The FlowMesh regtest: inactive with the lineage knob alone, gated at
    // R_key with -b3freshkeyheight.
    ArgsManager args;
    args.ForceSetArg("-b3modernregtest", "1");
    args.ForceSetArg("-b3flowmeshtest", "1");
    args.ForceSetArg("-b3freshlineageheight", "172");
    const auto lineage_only{CreateChainParams(args, ChainType::REGTEST)};
    BOOST_REQUIRE(lineage_only->GetConsensus().fresh_lineage);
    BOOST_CHECK(!lineage_only->GetConsensus().fresh_lineage->key_height);
    for (const int h : {150, 171, 172, 10'000}) {
        BOOST_CHECK(modern::GetPayloadTypeStatus(T12, 1, lineage_only->GetConsensus(), h) == PayloadTypeStatus::INACTIVE);
        BOOST_CHECK(!modern::IsMetadataCellActive(FINALITY_KEY_POLICY, 2, lineage_only->GetConsensus(), h));
    }
    args.ForceSetArg("-b3freshkeyheight", "150");
    const auto keyed{CreateChainParams(args, ChainType::REGTEST)};
    const Consensus::Params& k{keyed->GetConsensus()};
    BOOST_REQUIRE(k.fresh_lineage && k.fresh_lineage->key_height);
    BOOST_CHECK_EQUAL(*k.fresh_lineage->key_height, 150);
    BOOST_CHECK(modern::GetPayloadTypeStatus(T12, 1, k, 149) == PayloadTypeStatus::INACTIVE);
    BOOST_CHECK(!modern::IsMetadataCellActive(FINALITY_KEY_POLICY, 2, k, 149));
    for (const int h : {150, 171, 172, 10'000}) {
        BOOST_CHECK(modern::GetPayloadTypeStatus(T12, 1, k, h) == PayloadTypeStatus::ACTIVE);
        BOOST_CHECK(modern::IsMetadataCellActive(FINALITY_KEY_POLICY, 2, k, h));
    }
}

BOOST_FIXTURE_TEST_CASE(mempool_and_block_wiring, FreshKeyChainFixture)
{
    PrepareFinalityChain();
    const int M{m_M};
    const int R_KEY{M + 5};
    const int R_F{M + 40}; // never reached: this case is about the binding kind only
    ChainstateManager& chainman{*m_node.chainman};
    ProduceTo(M + 1, m_vk_a);

    // Fresh keys come from an IKM family of their own (the fixture's V1 keys
    // use another), so they are disjoint from every V1 binding.
    const bls::SecretKey fresh_a{BlsKey(101)};
    const bls::SecretKey fresh_b{BlsKey(102)};
    const bls::SecretKey fresh_d{BlsKey(104)};
    for (const bls::SecretKey* v1 : {&m_bls_a, &m_bls_b, &m_bls_c, &m_bls_d}) {
        for (const bls::SecretKey* fresh : {&fresh_a, &fresh_b, &fresh_d}) BOOST_REQUIRE(Pk(*v1) != Pk(*fresh));
    }

    const auto bind_a{MakeFreshBinding(m_validator_a, m_vk_a, fresh_a, 0)};
    const CMutableTransaction tx_a{RelayTx(4, {bind_a.cell}, {bind_a.record})};
    const CMutableTransaction record_only{RelayTx(4, {}, {bind_a.record})};
    const CMutableTransaction cell_only{RelayTx(4, {bind_a.cell}, {})};
    const auto in_tx{[](const CMutableTransaction& tx) {
        return " in transaction " + CTransaction{tx}.GetHash().ToString();
    }};
    const std::string inactive_cell{"inactive metadata policy 7 v2 at output 1"};

    // 1. Without R_key a fresh binding is refused exactly as before B-37: the
    //    (7, 2) cell is an inactive metadata policy, checked before the MPA.
    //    A bare type-12 record is a known-but-inactive MPA type (it was
    //    mpa-unknown-type before the type was registered): invalid either way.
    const auto refused_everywhere{[&] {
        CheckMempoolRefused(tx_a, "bad-metadata-cell", inactive_cell);
        CheckMempoolRefused(record_only, "bad-mpa", "mpa-inactive-type");
        CheckMempoolRefused(cell_only, "bad-metadata-cell", inactive_cell);
        BOOST_CHECK_EQUAL(ProbeWith(tx_a), "bad-metadata-cell, " + inactive_cell + in_tx(tx_a));
        BOOST_CHECK_EQUAL(ProbeWith(record_only), "bad-mpa, mpa-inactive-type" + in_tx(record_only));
        BOOST_CHECK_EQUAL(ProbeWith(cell_only), "bad-metadata-cell, " + inactive_cell + in_tx(cell_only));
    }};
    BOOST_REQUIRE(!chainman.GetConsensus().fresh_lineage);
    BOOST_CHECK(!chainman.GetConsensus().FreshKeyRulesActive(Tip()->nHeight + 1));
    refused_everywhere(); // no fresh lineage (every shipped network)
    MutableConsensus().fresh_lineage = Consensus::FreshLineageParams{
        .activation_height = R_F, .max_epoch_extension = SCALED_MAX_EXTENSION, .staging_signer = true};
    refused_everywhere(); // the pre-B-37 staging lineage (V1 keys)

    // 2. Key mode, below R_key: still inactive, in the mempool and in a block.
    MutableConsensus().fresh_lineage->key_height = R_KEY;
    refused_everywhere();
    ProduceTo(R_KEY - 2, m_vk_a);
    BOOST_CHECK(!chainman.GetConsensus().FreshKeyRulesActive(Tip()->nHeight + 1));
    refused_everywhere();
    Produce(m_vk_a);
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, R_KEY - 1);
    BOOST_REQUIRE(chainman.GetConsensus().FreshKeyRulesActive(Tip()->nHeight + 1));

    // 3. From R_key the bijection, the identity authorization and the proof
    //    of possession are consensus, in the mempool and in a block.
    CheckMempoolRefused(record_only, "bad-fresh-finality-key-binding", "fresh-key-orphan-evidence");
    CheckMempoolRefused(cell_only, "bad-fresh-finality-key-binding", "fresh-key-cell-without-evidence");
    BOOST_CHECK_EQUAL(ProbeWith(record_only),
                      "bad-fresh-finality-key-binding, fresh-key-orphan-evidence" + in_tx(record_only));
    BOOST_CHECK_EQUAL(ProbeWith(cell_only),
                      "bad-fresh-finality-key-binding, fresh-key-cell-without-evidence" + in_tx(cell_only));
    {
        const auto forged{MakeFreshBinding(m_validator_a, m_vk_a, fresh_a, 0, /*signer=*/&m_validator_b)};
        const CMutableTransaction tx{RelayTx(4, {forged.cell}, {forged.record})};
        CheckMempoolRefused(tx, "fresh-key-bad-bip340-signature");
        BOOST_CHECK_EQUAL(ProbeWith(tx), "bad-fresh-finality-key-evidence, fresh-key-bad-bip340-signature");
        // A real connect stores the block, refuses it and keeps the tip.
        SubmitExpectConnectFailure(NextBlockWith(tx, /*extra=*/1));
    }
    {
        const auto no_pop{MakeFreshBinding(m_validator_a, m_vk_a, fresh_a, 0, nullptr, /*pop_key=*/&fresh_b)};
        const CMutableTransaction tx{RelayTx(4, {no_pop.cell}, {no_pop.record})};
        CheckMempoolRefused(tx, "fresh-key-bad-pop");
        BOOST_CHECK_EQUAL(ProbeWith(tx), "bad-fresh-finality-key-evidence, fresh-key-bad-pop");
        SubmitExpectConnectFailure(NextBlockWith(tx, /*extra=*/2));
    }
    {
        // A V1 FINALITY_KEY bind signature over the same key and epoch-as-seq
        // never authorizes a fresh binding (distinct tagged hash).
        auto replay{bind_a};
        auto ev{*FreshFinalityKeyEvidence::Decode(replay.record.payload)};
        const uint256 v1_digest{modern::FinalityBindDigest(m_domain, m_vk_a, ev.bls_pubkey, 0)};
        uint256 aux{};
        BOOST_REQUIRE(m_validator_a.SignSchnorr(v1_digest, ev.bip340_sig, nullptr, aux));
        replay.record = modern::MakeFreshFinalityKeyRecord(ev);
        const CMutableTransaction tx{RelayTx(4, {replay.cell}, {replay.record})};
        CheckMempoolRefused(tx, "fresh-key-bad-bip340-signature");
        BOOST_CHECK_EQUAL(ProbeWith(tx), "bad-fresh-finality-key-evidence, fresh-key-bad-bip340-signature");
    }
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, R_KEY - 1);
    BOOST_CHECK_EQUAL(m_node.mempool->size(), 0U);

    // 4. Valid bindings enter the mempool. D's transaction carries a V1 first
    //    binding next to a fresh one: each matcher sees only its own pair, and
    //    the V1 semantic pre-check runs unchanged.
    {
        const auto res{Mempool(tx_a, /*test_accept=*/false)};
        BOOST_CHECK_MESSAGE(res.m_result_type == MempoolAcceptResult::ResultType::VALID,
                            res.m_state.GetRejectReason());
    }
    // Front-running (Codex review): with A's binding pending, B copies its
    // key and proof of possession. As a fresh binding under B's identity the
    // copy is consensus-invalid (the proof names A); as a V1 rebinding of B
    // to that key, V1 demands the bare IETF PoP, which no fresh binding
    // publishes. Neither can be mined ahead of A's binding to burn its key.
    {
        const FreshFinalityKeyEvidence pending{*FreshFinalityKeyEvidence::Decode(bind_a.record.payload)};
        FreshFinalityKeyParams params;
        params.bls_pubkey = pending.bls_pubkey;
        params.target_epoch = pending.target_epoch;
        FreshFinalityKeyEvidence copied{pending};
        copied.validator_key = m_vk_b;
        const uint256 digest{modern::FreshFinalityBindDigest(m_domain, m_vk_b, copied.bls_pubkey, copied.target_epoch)};
        uint256 aux{};
        BOOST_REQUIRE(m_validator_b.SignSchnorr(digest, copied.bip340_sig, nullptr, aux));
        const CMutableTransaction fresh_copy{RelayTx(12, {modern::MakeFreshFinalityKeyCell(m_vk_b, params)},
                                                     {modern::MakeFreshFinalityKeyRecord(copied)})};
        CheckMempoolRefused(fresh_copy, "fresh-key-bad-pop");
        BOOST_CHECK_EQUAL(ProbeWith(fresh_copy), "bad-fresh-finality-key-evidence, fresh-key-bad-pop");

        auto v1_copy{MakeBinding(m_validator_b, m_vk_b, &fresh_a, 1)};
        auto v1_ev{*modern::FinalityKeyEvidence::Decode(v1_copy.record.payload)};
        v1_ev.pop = pending.pop;
        const auto v1_enc{v1_ev.Encode()};
        v1_copy.record.payload.assign(v1_enc.begin(), v1_enc.end());
        const CMutableTransaction v1_tx{RelayTx(13, {v1_copy.cell}, {v1_copy.record})};
        const auto res{Mempool(v1_tx)};
        BOOST_CHECK(res.m_result_type == MempoolAcceptResult::ResultType::INVALID);
        BOOST_CHECK_EQUAL(res.m_state.GetRejectReason(), "finality-key-bad-pop");
        const std::string probe{ProbeWith(v1_tx)};
        BOOST_CHECK_MESSAGE(probe.find("bad-pop") != std::string::npos, probe);
        BOOST_CHECK_EQUAL(m_node.mempool->size(), 1U); // A's binding only
    }
    const auto v1_d{MakeBinding(m_validator_d, m_vk_d, &m_bls_d, 0)};
    const auto fresh_bind_d{MakeFreshBinding(m_validator_d, m_vk_d, fresh_d, 0)};
    const CMutableTransaction tx_d{RelayTx(5, {v1_d.cell, fresh_bind_d.cell}, {v1_d.record, fresh_bind_d.record})};
    {
        const CTransaction ctx{tx_d};
        std::vector<modern::FinalityKeyPair> v1_pairs;
        std::vector<FreshFinalityKeyPair> fresh_pairs;
        std::string err;
        BOOST_REQUIRE(modern::MatchFinalityKeyPairs(ctx, v1_pairs, err));
        BOOST_REQUIRE(modern::MatchFreshFinalityKeyPairs(ctx, fresh_pairs, err));
        BOOST_REQUIRE_EQUAL(v1_pairs.size(), 1U);
        BOOST_REQUIRE_EQUAL(fresh_pairs.size(), 1U);
        BOOST_CHECK_EQUAL(v1_pairs[0].cell_index, 1U);
        BOOST_CHECK_EQUAL(fresh_pairs[0].cell_index, 2U);
        // A V1 pre-check failure still wins with its own (policy) reason.
        const auto bad_seq{MakeBinding(m_validator_d, m_vk_d, &m_bls_d, 3)};
        const CMutableTransaction bad{
            RelayTx(5, {bad_seq.cell, fresh_bind_d.cell}, {bad_seq.record, fresh_bind_d.record})};
        const auto res_bad{Mempool(bad)};
        BOOST_CHECK(res_bad.m_result_type == MempoolAcceptResult::ResultType::INVALID);
        BOOST_CHECK(res_bad.m_state.GetResult() == TxValidationResult::TX_NOT_STANDARD);
        BOOST_CHECK_EQUAL(res_bad.m_state.GetRejectReason(), "finality-key-bad-first-seq");
        const auto res{Mempool(tx_d, /*test_accept=*/false)};
        BOOST_CHECK_MESSAGE(res.m_result_type == MempoolAcceptResult::ResultType::VALID,
                            res.m_state.GetRejectReason());
    }
    BOOST_CHECK_EQUAL(m_node.mempool->size(), 2U);

    // 5. The unchanged assembler mines both at R_key; the block connects.
    node::BlockAssembler::Options options;
    options.coinbase_output_script = CScript() << OP_TRUE;
    options.include_dummy_extranonce = true;
    options.modern_pos_validator_key = m_vk_a;
    const auto tmpl{node::BlockAssembler(chainman.ActiveChainstate(), m_node.mempool.get(), options).CreateNewBlock()};
    BOOST_REQUIRE(tmpl);
    CBlock block{tmpl->block};
    BOOST_REQUIRE_EQUAL(block.vtx.size(), 3U);
    for (const CMutableTransaction* expected : {&tx_a, &tx_d}) {
        const Txid txid{CTransaction{*expected}.GetHash()};
        BOOST_CHECK(std::any_of(block.vtx.begin() + 1, block.vtx.end(),
                                [&](const CTransactionRef& tx) { return tx->GetHash() == txid; }));
    }
    block.hashMerkleRoot = BlockMerkleRoot(block);
    Sign(block, m_validator_a);
    if (block.GetBlockTime() > GetTime()) SetMockTime(block.GetBlockTime());
    BOOST_REQUIRE_MESSAGE(Probe(block).empty(), Probe(block));
    BOOST_REQUIRE(Submit(block));
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, R_KEY);
    BOOST_CHECK_EQUAL(m_node.mempool->size(), 0U);
    {
        // The V1 index took D's V1 key and nothing else: no fresh key is a
        // V1 binding, and A's V1 record is untouched.
        const node::FinalityBindingIndex& index{Index()};
        const auto rec_d{index.Get(m_vk_d)};
        BOOST_REQUIRE(rec_d);
        BOOST_CHECK(rec_d->bls_pubkey == Pk(m_bls_d));
        const auto rec_a{index.Get(m_vk_a)};
        BOOST_REQUIRE(rec_a);
        BOOST_CHECK(rec_a->bls_pubkey == Pk(m_bls_a));
        for (const bls::SecretKey* fresh : {&fresh_a, &fresh_b, &fresh_d}) BOOST_CHECK(!index.OwnerOf(Pk(*fresh)));
    }

    // 6. A directly built block carrying B's binding (a later epoch target;
    //    windows are node-side, not consensus) connects too.
    const auto bind_b{MakeFreshBinding(m_validator_b, m_vk_b, fresh_b, 1)};
    const CMutableTransaction tx_b{MakeTx(8, {bind_b.cell}, {bind_b.record})};
    BOOST_CHECK_EQUAL(ProbeWith(tx_b), "");
    Produce(m_vk_a, {}, {tx_b});
    BOOST_CHECK_EQUAL(Tip()->nHeight, R_KEY + 1);
}

BOOST_AUTO_TEST_CASE(key_index_first_claim_inert_reuse_and_exact_undo)
{
    const Validator a{1}, b{2}, c{3}, d{4};
    std::vector<BlsPubkeyBytes> k;
    for (unsigned i = 0; i < 8; ++i) k.push_back(Pk(BlsKey(40 + i)));
    constexpr int MAX{std::numeric_limits<int>::max()};

    // One block per entry, claims in block order.
    const std::vector<std::pair<int, std::vector<FreshClaim>>> blocks{
        {10, {V1Claim(a, k[1]), V1Claim(b, k[2])}},            // V1 bindings
        {11, {NewClaim(b, k[1], 0), NewClaim(a, k[3], 0)}},    // B reuses A's V1 key (inert); A's genesis key counts
        {12, {NewClaim(c, k[4], 0), NewClaim(d, k[4], 0)}},    // one block, one key twice: C (first) wins
        {13, {V1Claim(d, k[5]), NewClaim(d, k[5], 0)}},        // one tx: its V1 pair claims first
        {14, {NewClaim(a, k[3], 2)}},                          // A's genesis key again, for epoch 2: inert
        {15, {}},                                              // an ordinary block
        {20, {NewClaim(a, k[6], 2)}},
        {25, {NewClaim(a, k[7], 2), V1Claim(b, k[6])}},        // A binds twice for 2; V1 claim of a fresh key: no trace
    };
    node::FreshKeyBindingIndex index;
    std::vector<node::FreshKeyBindingIndex> before;
    for (const auto& [height, claims] : blocks) {
        before.push_back(index);
        index.ConnectBlock(height, claims);
    }

    // Ever bound: every key, at its first claim, under either kind.
    const std::vector<std::pair<size_t, int>> first{{1, 10}, {2, 10}, {3, 11}, {4, 12}, {5, 13}, {6, 20}, {7, 25}};
    for (const auto& [i, height] : first) {
        BOOST_CHECK(index.EverBound(k[i]));
        BOOST_REQUIRE(index.FirstBoundHeight(k[i]));
        BOOST_CHECK_EQUAL(*index.FirstBoundHeight(k[i]), height);
    }
    BOOST_CHECK(!index.EverBound(k[0]));
    BOOST_CHECK(!index.FirstBoundHeight(k[0]));
    BOOST_CHECK_EQUAL(index.EverBoundCount(), 7U);

    // Counted fresh bindings: only claims that won their key.
    const auto only{[](const std::vector<node::FreshKeyBinding>& list, const BlsPubkeyBytes& pk, const int height) {
        return list.size() == 1 && list[0] == node::FreshKeyBinding{pk, height};
    }};
    BOOST_CHECK(index.Bindings(b.vk, 0, 0, MAX).empty());                // reuse of a V1 key is inert
    BOOST_CHECK(only(index.Bindings(a.vk, 0, 0, MAX), k[3], 11));
    BOOST_CHECK(only(index.Bindings(c.vk, 0, 0, MAX), k[4], 12));
    BOOST_CHECK(index.Bindings(d.vk, 0, 0, MAX).empty());                // lost k4 to C, k5 to its own V1 pair
    const auto twice{index.Bindings(a.vk, 2, 0, MAX)};                   // k3 for epoch 2 is inert
    BOOST_REQUIRE_EQUAL(twice.size(), 2U);
    BOOST_CHECK(twice[0] == (node::FreshKeyBinding{k[6], 20}));
    BOOST_CHECK(twice[1] == (node::FreshKeyBinding{k[7], 25}));
    BOOST_CHECK_EQUAL(index.BindingCount(), 4U);

    // Window bounds are inclusive on both ends.
    BOOST_CHECK(only(index.Bindings(a.vk, 2, 20, 20), k[6], 20));
    BOOST_CHECK(only(index.Bindings(a.vk, 2, 0, 24), k[6], 20));
    BOOST_CHECK(only(index.Bindings(a.vk, 2, 21, MAX), k[7], 25));
    BOOST_CHECK(only(index.Bindings(a.vk, 2, 25, 25), k[7], 25));
    BOOST_CHECK(index.Bindings(a.vk, 2, 26, MAX).empty());
    BOOST_CHECK(index.Bindings(a.vk, 2, 21, 24).empty());
    BOOST_CHECK(index.Bindings(a.vk, 1, 0, MAX).empty());

    // RPC listing: epoch filter, (validator, epoch) then chain order.
    BOOST_CHECK_EQUAL(index.BindingsFrom(0).size(), 4U);
    const auto from2{index.BindingsFrom(2)};
    BOOST_REQUIRE_EQUAL(from2.size(), 2U);
    BOOST_CHECK(from2[0] == (node::FreshKeyBindingEntry{a.vk, 2, {k[6], 20}}));
    BOOST_CHECK(from2[1] == (node::FreshKeyBindingEntry{a.vk, 2, {k[7], 25}}));
    BOOST_CHECK(index.BindingsFrom(3).empty());

    // Blocks 14 and 15 left no trace.
    BOOST_CHECK(before[5] == before[4]);

    // OwnFreshEpoch: the epoch of a validator's binding of a key only when
    // that binding was the key's first claim; a key claimed first elsewhere
    // (another validator, or a V1 pair) has none for the late claimant.
    BOOST_CHECK(index.OwnFreshEpoch(a.vk, k[3]) == std::optional<uint64_t>{0}); // its epoch-2 reuse is inert
    BOOST_CHECK(index.OwnFreshEpoch(a.vk, k[6]) == std::optional<uint64_t>{2});
    BOOST_CHECK(index.OwnFreshEpoch(c.vk, k[4]) == std::optional<uint64_t>{0});
    BOOST_CHECK(!index.OwnFreshEpoch(d.vk, k[4]));  // C claimed k4 first
    BOOST_CHECK(!index.OwnFreshEpoch(d.vk, k[5]));  // D's own V1 pair claimed k5 first
    BOOST_CHECK(!index.OwnFreshEpoch(b.vk, k[1]));  // A's V1 key
    BOOST_CHECK(!index.OwnFreshEpoch(b.vk, k[6]));  // B's V1 claim of a fresh key: A's
    BOOST_CHECK(!index.OwnFreshEpoch(a.vk, k[0]));  // never claimed

    // Incremental == rebuild: the same blocks into an empty index, and after
    // Clear(). (There is no undo: the tracker rebuilds after a disconnect.)
    {
        node::FreshKeyBindingIndex rebuilt;
        for (const auto& [height, claims] : blocks) rebuilt.ConnectBlock(height, claims);
        BOOST_CHECK(rebuilt == index);
        rebuilt.Clear();
        BOOST_CHECK(rebuilt == node::FreshKeyBindingIndex{});
        BOOST_CHECK_EQUAL(rebuilt.EverBoundCount(), 0U);
        for (const auto& [height, claims] : blocks) rebuilt.ConnectBlock(height, claims);
        BOOST_CHECK(rebuilt == index);
    }
    // A rebuild of a reorg-like replacement (20 and 25 replaced by a new 20)
    // keeps exactly the surviving claims.
    {
        node::FreshKeyBindingIndex replaced;
        for (const auto& [height, claims] : blocks) {
            if (height >= 20) break;
            replaced.ConnectBlock(height, claims);
        }
        replaced.ConnectBlock(20, {NewClaim(b, k[7], 2)});
        BOOST_CHECK(only(replaced.Bindings(b.vk, 2, 0, MAX), k[7], 20));
        BOOST_CHECK(replaced.Bindings(a.vk, 2, 0, MAX).empty());
        BOOST_CHECK(!replaced.EverBound(k[6]));
    }
}

BOOST_AUTO_TEST_CASE(block_key_claims_order_and_refusal)
{
    const Validator a{1}, b{2}, c{3};
    const auto k1{BlsKey(51)}, k2{BlsKey(52)}, k3{BlsKey(53)};
    CBlock block;
    {
        CMutableTransaction coinbase;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vout.emplace_back(0, CScript() << OP_TRUE);
        block.vtx.push_back(MakeTransactionRef(coinbase));
    }
    // tx1: A's V1 binding of k1, B's V1 revocation (no key), and two fresh
    // pairs: A claims k1 again (fresh) and C claims k3. tx2: B's fresh k2.
    CMutableTransaction tx1{BaseTx()};
    AddV1Pair(tx1, a, k1);
    AddV1Revocation(tx1, b, 1);
    AddPair(tx1, a, Build(a, k1, 0));
    AddPair(tx1, c, Build(c, k3, 0));
    CMutableTransaction tx2{BaseTx()};
    tx2.vin[0].prevout.n = 2;
    AddPair(tx2, b, Build(b, k2, 4));
    block.vtx.push_back(MakeTransactionRef(tx1));
    block.vtx.push_back(MakeTransactionRef(tx2));

    std::vector<FreshClaim> claims;
    std::string err;
    BOOST_REQUIRE(node::BlockKeyClaims(block, claims, err));
    BOOST_REQUIRE_EQUAL(claims.size(), 4U);
    // Per transaction: V1 nonzero pairs first, then fresh pairs in key order.
    BOOST_CHECK(claims[0] == V1Claim(a, Pk(k1)));
    const FreshClaim fresh_a{NewClaim(a, Pk(k1), 0)}, fresh_c{NewClaim(c, Pk(k3), 0)};
    BOOST_CHECK((claims[1] == fresh_a && claims[2] == fresh_c) || (claims[1] == fresh_c && claims[2] == fresh_a));
    BOOST_CHECK(claims[1].validator_key < claims[2].validator_key);
    BOOST_CHECK(claims[3] == NewClaim(b, Pk(k2), 4));

    node::FreshKeyBindingIndex index;
    index.ConnectBlock(50, claims);
    BOOST_CHECK_EQUAL(*index.FirstBoundHeight(Pk(k1)), 50);
    BOOST_CHECK(index.Bindings(a.vk, 0, 0, 100).empty()); // its own V1 pair claimed k1 first
    BOOST_CHECK_EQUAL(index.Bindings(c.vk, 0, 0, 100).size(), 1U);
    BOOST_CHECK_EQUAL(index.Bindings(b.vk, 4, 0, 100).size(), 1U);
    BOOST_CHECK_EQUAL(index.EverBoundCount(), 3U);

    // A block a matcher refuses yields no claims (a connected block never does).
    CMutableTransaction orphan{BaseTx()};
    orphan.vin[0].prevout.n = 3;
    orphan.mpa.push_back(modern::MakeFreshFinalityKeyRecord(Build(a, k2, 0).ev));
    CBlock bad{block};
    bad.vtx.push_back(MakeTransactionRef(orphan));
    BOOST_CHECK(!node::BlockKeyClaims(bad, claims, err));
    BOOST_CHECK_EQUAL(err, "fresh-key-orphan-evidence");
    BOOST_CHECK(claims.empty());
}

BOOST_AUTO_TEST_CASE(binding_windows_and_mempool_policy)
{
    using Status = node::FreshBindingWindow::Status;
    const Configs cfg;
    constexpr int R_KEY{Configs::R_KEY};
    constexpr int R_F{Configs::R_F};

    // No key mode (every shipped network, and the pre-B-37 staging lineage): never open.
    for (const Consensus::Params* p : {&cfg.production, &cfg.xpin, &cfg.fresh}) {
        BOOST_CHECK(!node::FreshKeyModeConfigured(*p));
        for (const int h : {0, R_KEY - 1, R_KEY, R_F - 1, R_F, 100'000}) {
            BOOST_CHECK(node::FreshBindingWindowAt(h, *p, std::nullopt).status == Status::NOT_ACTIVE);
        }
    }
    BOOST_CHECK(node::FreshKeyModeConfigured(cfg.keyed));
    BOOST_CHECK(node::FreshBindingWindowAt(R_KEY - 1, cfg.keyed, std::nullopt).status == Status::NOT_ACTIVE);
    // Genesis window [R_key, R_f - 1]: target 0, whatever the projection says.
    for (const int h : {R_KEY, R_KEY + 7, R_F - 1}) {
        const auto w{node::FreshBindingWindowAt(h, cfg.keyed, std::nullopt)};
        BOOST_CHECK(w.status == Status::OPEN);
        BOOST_CHECK_EQUAL(w.target_epoch, 0U);
        BOOST_CHECK_EQUAL(w.window_start, R_KEY);
    }
    // From R_f: e + 2 for the epoch e of the height, on a live lineage.
    BOOST_CHECK(node::FreshBindingWindowAt(R_F, cfg.keyed, std::nullopt).status == Status::LINEAGE_UNAVAILABLE);
    node::FreshLineageState live;
    live.bootstrapped = true;
    live.epoch = 0;
    live.epoch_starts = {R_F};
    {
        const auto w{node::FreshBindingWindowAt(R_F, cfg.keyed, live)};
        BOOST_CHECK(w.status == Status::OPEN);
        BOOST_CHECK_EQUAL(w.target_epoch, 2U);
        BOOST_CHECK_EQUAL(w.window_start, R_F);
    }
    live.epoch = 3;
    live.epoch_starts = {R_F, R_F + 30, R_F + 75, R_F + 106};
    {
        const auto w{node::FreshBindingWindowAt(R_F + 120, cfg.keyed, live)};
        BOOST_CHECK(w.status == Status::OPEN);
        BOOST_CHECK_EQUAL(w.target_epoch, 5U);
        BOOST_CHECK_EQUAL(w.window_start, R_F + 106);
    }
    {
        node::FreshLineageState broken{live};
        broken.lineage_broken = true;
        BOOST_CHECK(node::FreshBindingWindowAt(R_F + 120, cfg.keyed, broken).status == Status::LINEAGE_UNAVAILABLE);
        node::FreshLineageState unbootstrapped{live};
        unbootstrapped.bootstrapped = false;
        BOOST_CHECK(node::FreshBindingWindowAt(R_F + 120, cfg.keyed, unbootstrapped).status ==
                    Status::LINEAGE_UNAVAILABLE);
    }

    // Policy over one transaction's pairs.
    const Validator a{1}, c{3};
    const auto k1{BlsKey(61)}, k2{BlsKey(62)}, k3{BlsKey(63)};
    const auto pairs_of{[](const CMutableTransaction& tx) {
        std::vector<FreshFinalityKeyPair> pairs;
        std::string err;
        BOOST_REQUIRE(MatchFresh(tx, pairs, err));
        return pairs;
    }};
    const auto policy{[&](const CMutableTransaction& tx, const node::FreshKeyBindingIndex& index,
                          const node::FreshBindingWindow& w) {
        std::string reason;
        return node::CheckFreshKeyBindingPolicy(CTransaction{tx}, pairs_of(tx), index, w, reason) ? std::string{"ok"}
                                                                                                    : reason;
    }};
    const node::FreshKeyBindingIndex empty;
    node::FreshBindingWindow open0{Status::OPEN, 0, R_KEY};
    node::FreshBindingWindow open2{Status::OPEN, 2, R_F};
    node::FreshBindingWindow unavailable{Status::LINEAGE_UNAVAILABLE, 0, -1};
    node::FreshBindingWindow inactive{};

    CMutableTransaction genesis{BaseTx()};
    AddPair(genesis, a, Build(a, k1, 0));
    BOOST_CHECK_EQUAL(policy(genesis, empty, open0), "ok");
    BOOST_CHECK_EQUAL(policy(genesis, empty, open2), "fresh-key-binding-outside-window");
    BOOST_CHECK_EQUAL(policy(genesis, empty, unavailable), "fresh-key-lineage-unavailable");
    BOOST_CHECK_EQUAL(policy(genesis, empty, inactive), "fresh-key-binding-outside-window");
    // A key bound before, under either kind, comes first.
    node::FreshKeyBindingIndex v1_bound;
    v1_bound.ConnectBlock(30, {V1Claim(c, Pk(k1))});
    BOOST_CHECK_EQUAL(policy(genesis, v1_bound, open0), "fresh-key-ever-bound");
    BOOST_CHECK_EQUAL(policy(genesis, v1_bound, unavailable), "fresh-key-ever-bound");
    node::FreshKeyBindingIndex fresh_bound;
    fresh_bound.ConnectBlock(30, {NewClaim(c, Pk(k1), 0)});
    BOOST_CHECK_EQUAL(policy(genesis, fresh_bound, open0), "fresh-key-ever-bound");
    // The same key claimed twice by one transaction: by its V1 pair, or by
    // two fresh pairs.
    CMutableTransaction with_v1{BaseTx()};
    AddV1Pair(with_v1, a, k2);
    AddPair(with_v1, a, Build(a, k2, 0));
    BOOST_CHECK_EQUAL(policy(with_v1, empty, open0), "fresh-key-ever-bound");
    CMutableTransaction two{BaseTx()};
    AddPair(two, a, Build(a, k3, 0));
    AddPair(two, c, Build(c, k3, 0));
    BOOST_CHECK_EQUAL(policy(two, empty, open0), "fresh-key-ever-bound");
    // Two distinct keys of two members, both in the window: fine. A double
    // binding (one member, two keys, one epoch) is not refused here: the seat
    // rule voids it, and refusing the second one would let a first binding
    // made from an old backup win.
    CMutableTransaction members{BaseTx()};
    AddPair(members, a, Build(a, k2, 0));
    AddPair(members, c, Build(c, k3, 0));
    BOOST_CHECK_EQUAL(policy(members, empty, open0), "ok");
    CMutableTransaction twice{BaseTx()};
    AddPair(twice, a, Build(a, k2, 0));
    AddPair(twice, a, Build(a, k3, 0));
    BOOST_CHECK_EQUAL(policy(twice, empty, open0), "ok");
}

BOOST_FIXTURE_TEST_CASE(fresh_key_index_on_chain_and_mempool_policy, FreshKeyChainFixture)
{
    PrepareFinalityChain(/*min_finality_set=*/1, /*reorg_horizon=*/200, /*with_unbound_c=*/false,
                         /*with_bound_c=*/true, /*with_bound_d=*/true);
    const int M{m_M};
    const int R_KEY{M + 3};
    const int R_F{M + 11};
    BOOST_REQUIRE(modern::IsCheckpointHeight(R_F - 1, M, SCALED_INTERVAL));
    MutableConsensus().fresh_lineage = Consensus::FreshLineageParams{
        .activation_height = R_F, .max_epoch_extension = SCALED_MAX_EXTENSION, .staging_signer = true};
    MutableConsensus().fresh_lineage->key_height = R_KEY;
    const Consensus::Params& consensus{m_node.chainman->GetConsensus()};
    BOOST_REQUIRE(node::FreshKeyModeConfigured(consensus));

    // A shadow tracker over the same chain WITHOUT key mode (the pre-B-37
    // staging lineage): every V1 field matches the key-mode tracker at every
    // height, and its fresh key tracker is never stepped.
    Consensus::Params no_keys{consensus};
    no_keys.fresh_lineage->key_height.reset();
    BOOST_REQUIRE(!node::FreshKeyModeConfigured(no_keys));
    node::FinalityTracker shadow;
    const auto check_v1{[&]() {
        LOCK(cs_main);
        const CChain& chain{m_node.chainman->ActiveChain()};
        BOOST_REQUIRE(shadow.Sync(chain, m_node.chainman->m_blockman, no_keys, *chain.Tip()));
        const node::FinalityTracker& live{Finality()};
        BOOST_CHECK_MESSAGE(SameV1Fields(live.Current(), shadow.Current()),
                            "V1 state differs at height " << chain.Height());
        BOOST_CHECK(shadow.FreshKeys(no_keys) == nullptr);
        BOOST_CHECK(shadow.FreshKeys(consensus) == nullptr);
        BOOST_CHECK(live.FreshKeys(no_keys) == nullptr);
        BOOST_CHECK(live.FreshKeys(consensus) != nullptr);
    }};
    check_v1();

    // The corridor's V1 bindings are ever bound from their first claim.
    {
        const auto index{FreshKeysAtTip()};
        for (const bls::SecretKey* v1 : {&m_bls_a, &m_bls_b, &m_bls_c, &m_bls_d}) {
            BOOST_REQUIRE(index.FirstBoundHeight(Pk(*v1)));
            BOOST_CHECK_EQUAL(*index.FirstBoundHeight(Pk(*v1)), b3test::SYN_H + 1);
        }
        BOOST_CHECK_EQUAL(index.EverBoundCount(), 4U);
        BOOST_CHECK_EQUAL(index.BindingCount(), 0U);
    }
    while (Tip()->nHeight < R_KEY - 1) {
        Produce(m_vk_a);
        check_v1();
    }

    // Fresh keys from an IKM family of their own (disjoint from every V1 key).
    const bls::SecretKey fresh_a{BlsKey(111)}, fresh_k{BlsKey(112)}, fresh_d{BlsKey(113)};
    const bls::SecretKey fresh_d2{BlsKey(114)}, fresh_a2{BlsKey(115)}, fresh_x{BlsKey(116)};

    // 1. Next block R_key (genesis window, target 0).
    {
        // The member's own V1 key, and another member's, as a fresh key.
        const auto own{MakeFreshBinding(m_validator_a, m_vk_a, m_bls_a, 0)};
        CheckPolicyRefused(RelayTx(4, {own.cell}, {own.record}), "fresh-key-ever-bound");
        const auto other{MakeFreshBinding(m_validator_a, m_vk_a, m_bls_c, 0)};
        CheckPolicyRefused(RelayTx(4, {other.cell}, {other.record}), "fresh-key-ever-bound");
        // Only target 0 counts before R_f.
        for (const uint64_t target : {1, 2}) {
            const auto early{MakeFreshBinding(m_validator_a, m_vk_a, fresh_a, target)};
            CheckPolicyRefused(RelayTx(4, {early.cell}, {early.record}), "fresh-key-binding-outside-window");
        }
        // One key claimed by two members in one transaction.
        const auto twice_a{MakeFreshBinding(m_validator_a, m_vk_a, fresh_x, 0)};
        const auto twice_c{MakeFreshBinding(m_validator_c, m_vk_c, fresh_x, 0)};
        CheckPolicyRefused(RelayTx(4, {twice_a.cell, twice_c.cell}, {twice_a.record, twice_c.record}),
                           "fresh-key-ever-bound");
    }
    const auto bind_a{MakeFreshBinding(m_validator_a, m_vk_a, fresh_a, 0)};
    const CMutableTransaction tx_a{RelayTx(4, {bind_a.cell}, {bind_a.record})};
    {
        const auto res{Mempool(tx_a, /*test_accept=*/false)};
        BOOST_CHECK_MESSAGE(res.m_result_type == MempoolAcceptResult::ResultType::VALID,
                            res.m_state.GetRejectReason());
    }
    Produce(m_vk_a, {}, {tx_a});
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, R_KEY);
    check_v1();
    {
        const auto index{FreshKeysAtTip()};
        BOOST_CHECK_EQUAL(*index.FirstBoundHeight(Pk(fresh_a)), R_KEY);
        const auto list{index.Bindings(m_vk_a, 0, R_KEY, R_F - 1)};
        BOOST_REQUIRE_EQUAL(list.size(), 1U);
        BOOST_CHECK(list[0] == (node::FreshKeyBinding{Pk(fresh_a), R_KEY}));
    }

    // 2. A previously fresh-bound key is refused, for another member or again.
    {
        const auto again_b{MakeFreshBinding(m_validator_b, m_vk_b, fresh_a, 0)};
        CheckPolicyRefused(RelayTx(5, {again_b.cell}, {again_b.record}), "fresh-key-ever-bound");
        const auto again_a{MakeFreshBinding(m_validator_a, m_vk_a, fresh_a, 0)};
        CheckPolicyRefused(RelayTx(5, {again_a.cell}, {again_a.record}), "fresh-key-ever-bound");
    }

    // 3. A directly built block carrying reused keys is valid; the reuse is
    //    inert. In the same block C and D claim one new key: C's earlier
    //    transaction wins, D's other key counts.
    const auto reuse_v1{MakeFreshBinding(m_validator_b, m_vk_b, m_bls_a, 0)};
    const auto reuse_fresh{MakeFreshBinding(m_validator_b, m_vk_b, fresh_a, 0)};
    const CMutableTransaction tx_reuse{
        MakeTx(5, {reuse_v1.cell, reuse_fresh.cell}, {reuse_v1.record, reuse_fresh.record})};
    const auto c_k{MakeFreshBinding(m_validator_c, m_vk_c, fresh_k, 0)};
    const CMutableTransaction tx_c{MakeTx(8, {c_k.cell}, {c_k.record})};
    const auto d_k{MakeFreshBinding(m_validator_d, m_vk_d, fresh_k, 0)};
    const auto d_1{MakeFreshBinding(m_validator_d, m_vk_d, fresh_d, 0)};
    const CMutableTransaction tx_d{MakeTx(9, {d_k.cell, d_1.cell}, {d_k.record, d_1.record})};
    {
        const CBlock block{BuildPosBlock(m_vk_a, {}, {tx_reuse, tx_c, tx_d})};
        const auto position{[&](const CMutableTransaction& tx) {
            const Txid txid{CTransaction{tx}.GetHash()};
            const auto it{std::find_if(block.vtx.begin(), block.vtx.end(),
                                       [&](const CTransactionRef& t) { return t->GetHash() == txid; })};
            BOOST_REQUIRE(it != block.vtx.end());
            return it - block.vtx.begin();
        }};
        BOOST_REQUIRE(position(tx_c) < position(tx_d));
        if (block.GetBlockTime() > GetTime()) SetMockTime(block.GetBlockTime());
        BOOST_REQUIRE_MESSAGE(Probe(block).empty(), Probe(block));
        BOOST_REQUIRE(Submit(block));
    }
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, R_KEY + 1);
    check_v1();
    {
        const auto index{FreshKeysAtTip()};
        BOOST_CHECK(index.Bindings(m_vk_b, 0, 0, R_F).empty());
        BOOST_CHECK_EQUAL(*index.FirstBoundHeight(Pk(m_bls_a)), b3test::SYN_H + 1);
        BOOST_CHECK_EQUAL(*index.FirstBoundHeight(Pk(fresh_a)), R_KEY);
        const auto c_list{index.Bindings(m_vk_c, 0, R_KEY, R_F - 1)};
        BOOST_REQUIRE_EQUAL(c_list.size(), 1U);
        BOOST_CHECK(c_list[0] == (node::FreshKeyBinding{Pk(fresh_k), R_KEY + 1}));
        const auto d_list{index.Bindings(m_vk_d, 0, R_KEY, R_F - 1)};
        BOOST_REQUIRE_EQUAL(d_list.size(), 1U);
        BOOST_CHECK(d_list[0] == (node::FreshKeyBinding{Pk(fresh_d), R_KEY + 1}));
        BOOST_CHECK_EQUAL(index.BindingCount(), 3U);
        BOOST_CHECK_EQUAL(index.EverBoundCount(), 7U);
        // The V1 index never saw a fresh key.
        const node::FinalityBindingIndex& v1{Index()};
        for (const bls::SecretKey* fresh : {&fresh_a, &fresh_k, &fresh_d}) BOOST_CHECK(!v1.OwnerOf(Pk(*fresh)));
        BOOST_CHECK(v1.Get(m_vk_b)->bls_pubkey == Pk(m_bls_b));
    }

    // 4. D binds a second key for epoch 0 (the old-backup shape). The mempool
    //    does not refuse it (the seat rule voids the seat); both are counted.
    const auto d_2{MakeFreshBinding(m_validator_d, m_vk_d, fresh_d2, 0)};
    {
        const auto res{Mempool(SpendTx(CTransaction{tx_d}, {d_2.cell}, {d_2.record}, /*relay=*/true))};
        BOOST_CHECK_MESSAGE(res.m_result_type == MempoolAcceptResult::ResultType::VALID,
                            res.m_state.GetRejectReason());
    }
    Produce(m_vk_a, {}, {SpendTx(CTransaction{tx_d}, {d_2.cell}, {d_2.record}, /*relay=*/false)});
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, R_KEY + 2);
    check_v1();
    {
        const auto index{FreshKeysAtTip()};
        const auto all{index.Bindings(m_vk_d, 0, R_KEY, R_F - 1)};
        BOOST_REQUIRE_EQUAL(all.size(), 2U);
        BOOST_CHECK(all[0] == (node::FreshKeyBinding{Pk(fresh_d), R_KEY + 1}));
        BOOST_CHECK(all[1] == (node::FreshKeyBinding{Pk(fresh_d2), R_KEY + 2}));
        const auto late{index.Bindings(m_vk_d, 0, R_KEY + 2, R_F - 1)};
        BOOST_REQUIRE_EQUAL(late.size(), 1U);
        BOOST_CHECK(late[0].bls_pubkey == Pk(fresh_d2));
        const auto early{index.Bindings(m_vk_d, 0, R_KEY, R_KEY + 1)};
        BOOST_REQUIRE_EQUAL(early.size(), 1U);
        BOOST_CHECK(early[0].bls_pubkey == Pk(fresh_d));
        BOOST_CHECK(index.Bindings(m_vk_d, 1, 0, R_F).empty());
    }

    // 5. From R_f the window counts e + 2 only.
    while (Tip()->nHeight < R_F - 1) {
        Produce(m_vk_a);
        check_v1();
    }
    {
        const auto projected{ProjectedNext()};
        BOOST_REQUIRE(projected.fresh && projected.fresh->bootstrapped);
        BOOST_REQUIRE_EQUAL(projected.fresh->epoch, 0U);
        const CTransaction funding{tx_c};
        for (const uint64_t target : {0, 1, 3}) {
            const auto wrong{MakeFreshBinding(m_validator_a, m_vk_a, fresh_a2, target)};
            CheckPolicyRefused(SpendTx(funding, {wrong.cell}, {wrong.record}, /*relay=*/true),
                               "fresh-key-binding-outside-window");
        }
        const auto right{MakeFreshBinding(m_validator_a, m_vk_a, fresh_a2, 2)};
        const auto res{Mempool(SpendTx(funding, {right.cell}, {right.record}, /*relay=*/true))};
        BOOST_CHECK_MESSAGE(res.m_result_type == MempoolAcceptResult::ResultType::VALID,
                            res.m_state.GetRejectReason());
    }

    // 6. Reorg below the conflict block and back: the index follows exactly.
    const node::FreshKeyBindingIndex at_tip{FreshKeysAtTip()};
    const uint256 conflict_hash{ChainHashAt(R_KEY + 1)};
    {
        BlockValidationState state;
        CBlockIndex* index{WITH_LOCK(cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(conflict_hash))};
        BOOST_REQUIRE(index != nullptr);
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().InvalidateBlock(state, index));
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().ActivateBestChain(state));
    }
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, R_KEY);
    check_v1();
    {
        const auto index{FreshKeysAtTip()};
        for (const bls::SecretKey* gone : {&fresh_k, &fresh_d, &fresh_d2}) BOOST_CHECK(!index.EverBound(Pk(*gone)));
        BOOST_CHECK(index.EverBound(Pk(fresh_a)));
        BOOST_CHECK_EQUAL(index.BindingCount(), 1U);
    }
    {
        CBlockIndex* index{WITH_LOCK(cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(conflict_hash))};
        BOOST_REQUIRE(index != nullptr);
        {
            LOCK(cs_main);
            m_node.chainman->ActiveChainstate().ResetBlockFailureFlags(index);
            m_node.chainman->RecalculateBestHeader();
        }
        BlockValidationState state;
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().ActivateBestChain(state));
    }
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, R_F - 1);
    check_v1();
    BOOST_CHECK(FreshKeysAtTip() == at_tip);

    // 7. Incremental == rebuild: a new tracker, and a stand-alone fresh key
    //    tracker stepped block by block, equal the live index.
    {
        LOCK(cs_main);
        const CChain& chain{m_node.chainman->ActiveChain()};
        const node::BlockManager& blockman{m_node.chainman->m_blockman};
        node::FinalityTracker rebuilt;
        BOOST_REQUIRE(rebuilt.Sync(chain, blockman, consensus, *chain.Tip()));
        BOOST_REQUIRE(rebuilt.FreshKeys(consensus) != nullptr);
        BOOST_CHECK(*rebuilt.FreshKeys(consensus) == at_tip);
        BOOST_CHECK(SameV1Fields(rebuilt.Current(), Finality().Current()));

        node::FreshKeyBindingTracker stepped;
        BOOST_REQUIRE(stepped.Sync(chain, blockman, consensus, *chain[R_KEY - 1]));
        for (int h{R_KEY}; h <= chain.Height(); ++h) {
            CBlock block;
            BOOST_REQUIRE(blockman.ReadBlock(block, *chain[h]));
            stepped.BlockConnected(block, *chain[h], consensus);
            BOOST_REQUIRE(stepped.Synced(chain[h]->GetBlockHash()));
        }
        BOOST_CHECK(stepped.Index() == at_tip);
        // Out of step (a gap): dirty, and Sync() rebuilds the same index.
        node::FreshKeyBindingTracker gap;
        BOOST_REQUIRE(gap.Sync(chain, blockman, consensus, *chain[R_KEY - 1]));
        CBlock block;
        BOOST_REQUIRE(blockman.ReadBlock(block, *chain[R_KEY + 1]));
        gap.BlockConnected(block, *chain[R_KEY + 1], consensus);
        BOOST_CHECK(!gap.Synced(chain[R_KEY + 1]->GetBlockHash()));
        BOOST_REQUIRE(gap.Sync(chain, blockman, consensus, *chain.Tip()));
        BOOST_CHECK(gap.Index() == at_tip);
    }
}

// ---------------------------------------------------------- committees

BOOST_AUTO_TEST_CASE(seat_rules_and_committee_builder)
{
    constexpr CAmount U{modern::FINALITY_WEIGHT_UNIT};
    const Validator a{1}, b{2}, c{3}, d{4}, e{5}, f{6}, g{7};
    // Snapshot(h): A-F hold V1 bindings (V1 keys BlsKey(170..175)); F then
    // revokes; G has none; E's stake is below one weight unit.
    node::FinalityBindingIndex v1;
    std::vector<FreshClaim> v1_claims;
    {
        std::vector<node::FinalityBindingIndex::Transition> bound;
        unsigned i{170};
        for (const Validator* v : {&a, &b, &c, &d, &e, &f}) {
            modern::BindingRecord record;
            record.bls_pubkey = Pk(BlsKey(i++));
            record.height = 10;
            bound.push_back({v->vk, record});
            v1_claims.push_back(V1Claim(*v, record.bls_pubkey));
        }
        v1.ConnectBlock(10, bound);
        modern::BindingRecord revoked;
        revoked.seq = 1;
        revoked.height = 11;
        v1.ConnectBlock(11, {{f.vk, revoked}});
    }
    const std::map<node::ValidatorKey, CAmount> weights{{a.vk, 15 * U}, {b.vk, 1 * U}, {c.vk, 10 * U},
                                                        {d.vk, 8 * U},  {e.vk, U / 2}, {f.vk, 5 * U},
                                                        {g.vk, 5 * U}};

    const auto fa{BlsKey(181)}, fb{BlsKey(182)}, fb_one{BlsKey(183)}, fc1{BlsKey(184)}, fc2{BlsKey(185)};
    const auto fd_early{BlsKey(186)}, fd_late{BlsKey(187)}, fe{BlsKey(188)}, ff{BlsKey(189)}, fg{BlsKey(190)};
    // Bindings around the window [100, 129] for target 2.
    node::FreshKeyBindingIndex fresh;
    fresh.ConnectBlock(10, v1_claims);
    fresh.ConnectBlock(99, {NewClaim(d, Pk(fd_early), 2)});
    fresh.ConnectBlock(100, {NewClaim(a, Pk(fa), 2), NewClaim(c, Pk(fc1), 2), NewClaim(b, Pk(fb_one), 1)});
    fresh.ConnectBlock(110, {NewClaim(c, Pk(fc2), 2), NewClaim(e, Pk(fe), 2), NewClaim(f, Pk(ff), 2),
                             NewClaim(g, Pk(fg), 2)});
    // Reuse is inert: D claims A's fresh key and A's V1 key.
    fresh.ConnectBlock(120, {NewClaim(d, Pk(fa), 2), NewClaim(d, Pk(BlsKey(170)), 2)});
    fresh.ConnectBlock(129, {NewClaim(b, Pk(fb), 2)});
    fresh.ConnectBlock(130, {NewClaim(d, Pk(fd_late), 2)});

    // Seats: one binding seats, none sits out, two void; bounds inclusive.
    {
        const auto seat{node::FreshSeatFor(fresh, a.vk, 2, 100, 129)};
        BOOST_CHECK(seat.Key() == Opt(fa));
        BOOST_CHECK(!seat.voided);
        BOOST_CHECK(node::FreshSeatFor(fresh, b.vk, 2, 100, 129).Key() == Opt(fb));
        BOOST_CHECK(node::FreshSeatFor(fresh, b.vk, 2, 100, 128).pubkeys.empty());
        BOOST_CHECK(node::FreshSeatFor(fresh, b.vk, 1, 100, 129).Key() == Opt(fb_one)); // another epoch's seat
    }
    {
        const auto seat{node::FreshSeatFor(fresh, c.vk, 2, 100, 129)};
        BOOST_CHECK(seat.voided);
        BOOST_CHECK(!seat.Key());
        BOOST_CHECK(seat.pubkeys == (std::vector<BlsPubkeyBytes>{Pk(fc1), Pk(fc2)}));
        BOOST_CHECK(node::FreshSeatFor(fresh, c.vk, 2, 100, 109).Key() == Opt(fc1));
    }
    {
        const auto seat{node::FreshSeatFor(fresh, d.vk, 2, 100, 129)};
        BOOST_CHECK(seat.pubkeys.empty()); // before / after the window, reuses inert
        BOOST_CHECK(!seat.voided);
        BOOST_CHECK(node::FreshSeatFor(fresh, d.vk, 2, 99, 129).Key() == Opt(fd_early));
        BOOST_CHECK(node::FreshSeatFor(fresh, d.vk, 2, 99, 130).voided);
    }

    // Committee(Snapshot(129)) for target 2 over [100, 129]: A and B on their
    // fresh keys. C void, D out, E below one unit, F revoked, G not in Snapshot.
    const auto committee{node::BuildFreshCommittee(weights, v1, fresh, node::FreshCommitteeRequest{129, 2, 100})};
    BOOST_REQUIRE(committee);
    {
        node::FinalityBindingIndex keys;
        modern::BindingRecord ra, rb;
        ra.bls_pubkey = Pk(fa);
        rb.bls_pubkey = Pk(fb);
        keys.ConnectBlock(1, {{a.vk, ra}, {b.vk, rb}});
        const std::map<node::ValidatorKey, CAmount> ab{{a.vk, 15 * U}, {b.vk, 1 * U}};
        const auto expected{node::ValidatorSetSnapshot::Build(0, ab, keys)};
        BOOST_REQUIRE(expected);
        BOOST_CHECK(*committee == *expected);
        BOOST_CHECK(committee->SetHash() == expected->SetHash());
    }
    BOOST_CHECK_EQUAL(committee->Size(), 2U);
    BOOST_CHECK_EQUAL(committee->TotalWeight(), 16U);
    BOOST_CHECK_EQUAL(committee->Epoch(), 0U);
    for (const auto& m : committee->Members()) {
        for (unsigned i{170}; i < 176; ++i) BOOST_CHECK(m.bls_pubkey != Pk(BlsKey(i))); // never a V1 key
    }
    // The window's upper end is the snapshot height, its lower end window_lo.
    {
        const auto earlier{node::BuildFreshCommittee(weights, v1, fresh, node::FreshCommitteeRequest{128, 2, 100})};
        BOOST_REQUIRE(earlier);
        BOOST_CHECK_EQUAL(earlier->Size(), 1U);
        BOOST_CHECK(SnapKey(*earlier, a.vk) == Opt(fa));
        const auto shifted{node::BuildFreshCommittee(weights, v1, fresh, node::FreshCommitteeRequest{109, 2, 99})};
        BOOST_REQUIRE(shifted);
        BOOST_CHECK_EQUAL(shifted->Size(), 3U);
        BOOST_CHECK(SnapKey(*shifted, a.vk) == Opt(fa));
        BOOST_CHECK(SnapKey(*shifted, c.vk) == Opt(fc1));
        BOOST_CHECK(SnapKey(*shifted, d.vk) == Opt(fd_early));
    }
    // Nobody seated: no committee.
    BOOST_CHECK(!node::BuildFreshCommittee(weights, v1, fresh, node::FreshCommitteeRequest{129, 7, 100}));
    BOOST_CHECK(!node::BuildFreshCommittee(weights, v1, fresh, node::FreshCommitteeRequest{98, 2, 100}));
}

BOOST_AUTO_TEST_CASE(projection_seat_rules_windows_void_and_carry_over)
{
    constexpr CAmount U{modern::FINALITY_WEIGHT_UNIT};
    constexpr int R_KEY{241};
    constexpr int R_F{262};
    constexpr int E{30};
    const uint256 PARENT{uint256{"abababababababababababababababababababababababababababababababab"}};
    Consensus::Params keyed{Params().GetConsensus()};
    {
        Consensus::ModernPosParams pos{};
        pos.finality_epoch_blocks = E;
        pos.checkpoint_interval = 5;
        pos.checkpoint_depth = 3;
        pos.max_epoch_extension = E;
        pos.min_finality_set = 2;
        keyed.modern_pos = pos;
    }
    keyed.legacy_final_hash = uint256::ONE;
    keyed.fresh_lineage = Consensus::FreshLineageParams{
        .activation_height = R_F, .max_epoch_extension = 1280, .staging_signer = true};
    keyed.fresh_lineage->key_height = R_KEY;
    Consensus::Params unkeyed{keyed};
    unkeyed.fresh_lineage->key_height.reset();

    // Snapshot: A, B, C, D with V1 keys BlsKey(170..173), weights 15/1/10/8.
    const Validator a{1}, b{2}, c{3}, d{4};
    node::FinalityBindingIndex v1;
    std::vector<FreshClaim> v1_claims;
    {
        std::vector<node::FinalityBindingIndex::Transition> bound;
        unsigned i{170};
        for (const Validator* v : {&a, &b, &c, &d}) {
            modern::BindingRecord record;
            record.bls_pubkey = Pk(BlsKey(i++));
            record.height = 10;
            bound.push_back({v->vk, record});
            v1_claims.push_back(V1Claim(*v, record.bls_pubkey));
        }
        v1.ConnectBlock(10, bound);
    }
    const std::map<node::ValidatorKey, CAmount> weights{{a.vk, 15 * U}, {b.vk, 1 * U}, {c.vk, 10 * U}, {d.vk, 8 * U}};

    const auto g_a{BlsKey(201)}, g_b{BlsKey(202)}, g_c{BlsKey(203)}, g_d1{BlsKey(204)}, g_d2{BlsKey(205)};
    const auto c_early{BlsKey(206)}, c_late0{BlsKey(207)}, b_one{BlsKey(208)}, a_early2{BlsKey(209)};
    const auto s_a{BlsKey(211)}, s_b1{BlsKey(212)}, s_b2{BlsKey(213)}, s_d{BlsKey(214)}, s_c_late{BlsKey(215)};
    const auto t_a{BlsKey(221)};                   // names 3 alone: below the floor
    const auto u_a{BlsKey(231)}, u_b{BlsKey(232)}; // name 4
    node::FreshKeyBindingIndex fresh;
    fresh.ConnectBlock(10, v1_claims);
    // Genesis window [R_key, R_f - 1] (target 0).
    fresh.ConnectBlock(R_KEY - 1, {NewClaim(c, Pk(c_early), 0)}); // before the window
    fresh.ConnectBlock(R_KEY, {NewClaim(a, Pk(g_a), 0), NewClaim(b, Pk(g_b), 0)});
    fresh.ConnectBlock(250, {NewClaim(d, Pk(g_d1), 0), NewClaim(b, Pk(b_one), 1)}); // target 1 never counts
    fresh.ConnectBlock(255, {NewClaim(d, Pk(g_d2), 0)});                            // D's second: void
    fresh.ConnectBlock(R_F - 1, {NewClaim(c, Pk(g_c), 0), NewClaim(a, Pk(a_early2), 2)});
    // Epoch 0, [R_f, start_1 - 1] = [262, 291] fixes Set'_2.
    fresh.ConnectBlock(R_F, {NewClaim(c, Pk(c_late0), 0), NewClaim(a, Pk(s_a), 2)});
    fresh.ConnectBlock(270, {NewClaim(b, Pk(s_b1), 2), NewClaim(d, Pk(s_d), 2), NewClaim(c, Pk(g_a), 2)});
    fresh.ConnectBlock(R_F + E - 1, {NewClaim(b, Pk(s_b2), 2)});     // B's second: void
    fresh.ConnectBlock(R_F + E, {NewClaim(c, Pk(s_c_late), 2)});     // after the window
    fresh.ConnectBlock(300, {NewClaim(a, Pk(t_a), 3)});             // [292, 321] fixes Set'_3
    fresh.ConnectBlock(330, {NewClaim(a, Pk(u_a), 4), NewClaim(b, Pk(u_b), 4)}); // [322, 351] fixes Set'_4

    std::vector<node::FreshCommitteeRequest> requests;
    int snapshot_calls{0};
    const node::FreshSnapshotAtFn snapshot_at{[&](int) {
        ++snapshot_calls;
        return node::ValidatorSetSnapshot::Build(0, weights, v1);
    }};
    const node::FreshCommitteeAtFn committee_at{[&](const node::FreshCommitteeRequest& r) {
        requests.push_back(r);
        return node::BuildFreshCommittee(weights, v1, fresh, r);
    }};
    // Genesis at R_f, then three rotations, each epoch's handover certified;
    // one block early nothing rotates.
    const auto run{[&](const Consensus::Params& params, const node::FreshCommitteeAtFn& fn) {
        std::vector<node::FreshLineageState> states;
        std::optional<node::FreshLineageState> s;
        node::ProjectFreshLineage(s, R_F - 1, PARENT, params, snapshot_at, fn);
        BOOST_CHECK(!s.has_value());
        node::ProjectFreshLineage(s, R_F, PARENT, params, snapshot_at, fn);
        BOOST_REQUIRE(s && s->bootstrapped);
        states.push_back(*s);
        for (const int h : {R_F + E, R_F + 2 * E, R_F + 3 * E}) {
            s->handover_certified = true;
            node::ProjectFreshLineage(s, h - 1, PARENT, params, snapshot_at, fn);
            BOOST_CHECK_EQUAL(s->epoch, states.size() - 1);
            node::ProjectFreshLineage(s, h, PARENT, params, snapshot_at, fn);
            BOOST_REQUIRE_EQUAL(s->epoch, states.size());
            BOOST_CHECK_EQUAL(s->epoch_starts.back(), h);
            states.push_back(*s);
        }
        return states;
    }};

    const auto states{run(keyed, committee_at)};
    BOOST_CHECK_EQUAL(snapshot_calls, 0);
    BOOST_REQUIRE_EQUAL(requests.size(), 4U);
    BOOST_CHECK(requests[0] == (node::FreshCommitteeRequest{R_F - 1, 0, R_KEY}));
    BOOST_CHECK(requests[1] == (node::FreshCommitteeRequest{R_F + E - 1, 2, R_F}));
    BOOST_CHECK(requests[2] == (node::FreshCommitteeRequest{R_F + 2 * E - 1, 3, R_F + E}));
    BOOST_CHECK(requests[3] == (node::FreshCommitteeRequest{R_F + 3 * E - 1, 4, R_F + 2 * E}));

    // Genesis: Set'_0 seats A, B, C on their genesis keys (C's bindings
    // before and after the window ignored), D's two bindings void its seat;
    // Set'_1 is the same members (B's binding naming 1 is unused).
    const auto& s0{states[0]};
    BOOST_CHECK(s0.genesis == s0.current);
    BOOST_CHECK_EQUAL(s0.current->Size(), 3U);
    BOOST_CHECK(SeatKey(s0.current, a.vk) == Opt(g_a));
    BOOST_CHECK(SeatKey(s0.current, b.vk) == Opt(g_b));
    BOOST_CHECK(SeatKey(s0.current, c.vk) == Opt(g_c));
    BOOST_CHECK(!SeatKey(s0.current, d.vk));
    BOOST_CHECK(s0.next->members == s0.current->members);
    BOOST_CHECK_EQUAL(s0.next->Epoch(), 1U);
    BOOST_CHECK(s0.next->header == modern::RulesetV2Header(s0.current->members->Header(), 1));
    BOOST_CHECK(SeatKey(s0.next, b.vk) == Opt(g_b));
    BOOST_CHECK_EQUAL(s0.current->TotalWeight(), 26U);
    {
        const auto domain{modern::ModernChainDomain(keyed.hashGenesisBlock, uint256::ONE)};
        BOOST_REQUIRE(domain);
        BOOST_CHECK(s0.lineage_id ==
                    modern::FinalityLineageIdV2(*domain, R_F - 1, PARENT, s0.current->set_hash));
    }
    // Rotation into 1: Set'_2 from [R_f, start_1 - 1]: A (its binding naming
    // 2 before the window ignored) and D on new keys; B void; C sits out (a
    // reused key and a binding one block late do not count).
    const auto& s1{states[1]};
    BOOST_CHECK(s1.epoch_starts == (std::vector<int>{R_F, R_F + E}));
    BOOST_CHECK(s1.previous->set_hash == s0.current->set_hash);
    BOOST_CHECK(s1.current->set_hash == s0.next->set_hash);
    BOOST_CHECK_EQUAL(s1.next->Epoch(), 2U);
    BOOST_CHECK_EQUAL(s1.next->Size(), 2U);
    BOOST_CHECK(SeatKey(s1.next, a.vk) == Opt(s_a));
    BOOST_CHECK(SeatKey(s1.next, d.vk) == Opt(s_d));
    BOOST_CHECK(!SeatKey(s1.next, b.vk));
    BOOST_CHECK(!SeatKey(s1.next, c.vk));
    // Rotation into 2: one binding naming 3 is below the floor, so Set'_3 is
    // the carry-over of Set'_2 (its members and keys, stamped 3).
    const auto& s2{states[2]};
    BOOST_CHECK(s2.current->set_hash == s1.next->set_hash);
    BOOST_CHECK(s2.next->members == s2.current->members);
    BOOST_CHECK_EQUAL(s2.next->Epoch(), 3U);
    BOOST_CHECK(SeatKey(s2.next, a.vk) == Opt(s_a));
    BOOST_CHECK(SeatKey(s2.next, d.vk) == Opt(s_d));
    // Rotation into 3: Set'_4 from [start_2, start_3 - 1] on new keys again.
    const auto& s3{states[3]};
    BOOST_CHECK(s3.current->set_hash == s2.next->set_hash);
    BOOST_CHECK_EQUAL(s3.next->Size(), 2U);
    BOOST_CHECK(SeatKey(s3.next, a.vk) == Opt(u_a));
    BOOST_CHECK(SeatKey(s3.next, b.vk) == Opt(u_b));
    BOOST_CHECK(s3.previous->set_hash == s1.next->set_hash);    // Set'_2
    BOOST_CHECK(s3.previous2->set_hash == s1.current->set_hash); // Set'_1

    // Without the seat rules the projection is exactly the old one: no
    // committee function, or no R_key (the function is then never asked).
    requests.clear();
    snapshot_calls = 0;
    const auto plain{run(unkeyed, {})};
    const auto keyed_without_fn{run(keyed, {})};
    const auto unkeyed_with_fn{run(unkeyed, committee_at)};
    BOOST_CHECK(requests.empty());
    BOOST_CHECK_EQUAL(snapshot_calls, 3 * 4);
    for (size_t i{0}; i < plain.size(); ++i) {
        BOOST_CHECK(SameFreshState(plain[i], keyed_without_fn[i]));
        BOOST_CHECK(SameFreshState(plain[i], unkeyed_with_fn[i]));
    }
    {
        const auto v1_snap{node::ValidatorSetSnapshot::Build(0, weights, v1)};
        BOOST_REQUIRE(v1_snap);
        BOOST_CHECK(plain[0].current->header == modern::RulesetV2Header(v1_snap->Header(), 0));
        BOOST_CHECK(plain[0].lineage_id != s0.lineage_id);
    }

    // Genesis below the floor stays unbootstrapped: no fallback to V1 keys.
    {
        node::FreshKeyBindingIndex only_a;
        only_a.ConnectBlock(10, v1_claims);
        only_a.ConnectBlock(R_KEY, {NewClaim(a, Pk(g_a), 0)});
        snapshot_calls = 0;
        const node::FreshCommitteeAtFn one{[&](const node::FreshCommitteeRequest& r) {
            return node::BuildFreshCommittee(weights, v1, only_a, r);
        }};
        std::optional<node::FreshLineageState> s;
        node::ProjectFreshLineage(s, R_F, PARENT, keyed, snapshot_at, one);
        BOOST_REQUIRE(s.has_value());
        BOOST_CHECK(!s->bootstrapped);
        BOOST_CHECK(!s->current && !s->next);
        s->handover_certified = true; // an unbootstrapped lineage never rotates
        node::ProjectFreshLineage(s, R_F + E, PARENT, keyed, snapshot_at, one);
        BOOST_CHECK(!s->bootstrapped);
        BOOST_CHECK_EQUAL(s->epoch, 0U);
        std::optional<node::FreshLineageState> none;
        const node::FreshCommitteeAtFn nobody{[](const node::FreshCommitteeRequest&) {
            return std::optional<node::ValidatorSetSnapshot>{};
        }};
        node::ProjectFreshLineage(none, R_F, PARENT, keyed, snapshot_at, nobody);
        BOOST_REQUIRE(none.has_value());
        BOOST_CHECK(!none->bootstrapped);
        BOOST_CHECK_EQUAL(snapshot_calls, 0);
    }
}

BOOST_FIXTURE_TEST_CASE(b37_committees_on_chain, FreshCommitteeChainFixture)
{
    // Four validators (A 15, B 1, C 10, D 8), MIN_FINALITY_SET = 2.
    PrepareFinalityChain(/*min_finality_set=*/2, /*reorg_horizon=*/200, /*with_unbound_c=*/false,
                         /*with_bound_c=*/true, /*with_bound_d=*/true);
    const int M{m_M};
    const int R_KEY{M + 3};
    const int R_F{M + 11};
    const int G{R_F - 1};
    const int E{SCALED_E};
    BOOST_REQUIRE(modern::IsCheckpointHeight(G, M, SCALED_INTERVAL));
    MutableConsensus().fresh_lineage = Consensus::FreshLineageParams{
        .activation_height = R_F, .max_epoch_extension = SCALED_MAX_EXTENSION, .staging_signer = true};
    MutableConsensus().fresh_lineage->key_height = R_KEY;
    const Consensus::Params& consensus{m_node.chainman->GetConsensus()};
    BOOST_REQUIRE(node::FreshKeyModeConfigured(consensus));
    const std::vector<const bls::SecretKey*> v1_keys{&m_bls_a, &m_bls_b, &m_bls_c, &m_bls_d};

    // Fresh keys from an IKM family of their own.
    const bls::SecretKey g_a{BlsKey(131)}, g_b{BlsKey(132)}, g_c{BlsKey(133)}, g_d{BlsKey(134)}; // genesis
    const bls::SecretKey b_one{BlsKey(135)};   // B names epoch 1: never counts
    const bls::SecretKey b_early{BlsKey(136)}; // B names epoch 2 before its window
    const bls::SecretKey s_a{BlsKey(141)}, s_c1{BlsKey(142)}, s_c2{BlsKey(143)}, s_d{BlsKey(144)}; // Set'_2
    const bls::SecretKey d_late{BlsKey(145)};  // D names epoch 2 one block after its window
    CheckV1AgainstReference();

    // 1. Genesis bindings at R_key (B also names epochs 1 and 2 here).
    ProduceTo(R_KEY - 1, m_vk_a);
    CMutableTransaction tx_genesis;
    {
        std::vector<CScript> cells;
        std::vector<CMpaRecord> records;
        const auto add{[&](const CKey& id, const ValidatorKeyBytes& vk, const bls::SecretKey& sk, const uint64_t e) {
            const auto b{MakeFreshBinding(id, vk, sk, e)};
            cells.push_back(b.cell);
            records.push_back(b.record);
        }};
        add(m_validator_a, m_vk_a, g_a, 0);
        add(m_validator_b, m_vk_b, g_b, 0);
        add(m_validator_c, m_vk_c, g_c, 0);
        add(m_validator_d, m_vk_d, g_d, 0);
        add(m_validator_b, m_vk_b, b_one, 1);
        add(m_validator_b, m_vk_b, b_early, 2);
        tx_genesis = MakeTx(4, cells, records);
    }
    Produce(m_vk_a, {}, {tx_genesis});
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, R_KEY);

    // 2. Genesis at R_f: Set'_0 = Set'_1 = the four members on their genesis
    //    keys, disjoint from the V1 keys of the same members.
    ProduceTo(R_F, m_vk_a);
    CheckV1AgainstReference();
    const auto genesis_state{Fresh()};
    BOOST_REQUIRE(genesis_state && genesis_state->bootstrapped);
    const auto set0{genesis_state->current};
    const auto set1{genesis_state->next};
    BOOST_REQUIRE(set0 && set1);
    BOOST_CHECK_EQUAL(set0->Size(), 4U);
    BOOST_CHECK(SeatKey(set0, m_vk_a) == Opt(g_a));
    BOOST_CHECK(SeatKey(set0, m_vk_b) == Opt(g_b));
    BOOST_CHECK(SeatKey(set0, m_vk_c) == Opt(g_c));
    BOOST_CHECK(SeatKey(set0, m_vk_d) == Opt(g_d));
    BOOST_CHECK(set1->members == set0->members);
    BOOST_CHECK_EQUAL(set1->Epoch(), 1U);
    {
        const auto v1_set{FinalityState().current};
        BOOST_REQUIRE(v1_set);
        BOOST_CHECK_EQUAL(set0->TotalWeight(), v1_set->TotalWeight());
        for (const auto& fresh_member : set0->members->Members()) {
            for (const auto& v1_member : v1_set->Members()) {
                BOOST_CHECK(fresh_member.bls_pubkey != v1_member.bls_pubkey);
            }
        }
        BOOST_CHECK(set0->set_hash != modern::ValidatorSetHash(modern::RulesetV2Header(v1_set->Header(), 0)));
    }

    // 3. Bindings naming 2 in [R_f, start_1 - 1]: A, C and D; B none.
    CMutableTransaction tx_two;
    {
        const auto ba{MakeFreshBinding(m_validator_a, m_vk_a, s_a, 2)};
        const auto bc{MakeFreshBinding(m_validator_c, m_vk_c, s_c1, 2)};
        const auto bd{MakeFreshBinding(m_validator_d, m_vk_d, s_d, 2)};
        tx_two = SpendTx(CTransaction{tx_genesis}, {ba.cell, bc.cell, bd.cell}, {ba.record, bc.record, bd.record},
                         /*relay=*/false);
    }
    Produce(m_vk_a, {}, {tx_two});

    // 4. Epoch-0 handover (checkpoint G + 5): the V1 keys of the same four
    //    members are refused; the fresh keys (A, C, D: weight 33) certify.
    const int cp0{G + 5};
    ProduceTo(cp0 + SCALED_DEPTH - 1, m_vk_a);
    {
        const auto fb{FreshFbForNext(cp0)};
        BOOST_CHECK_EQUAL(fb.epoch, 0U);
        BOOST_CHECK(fb.validator_set_hash == set1->set_hash);
        const auto set{SigningSetForNext(0)};
        BOOST_CHECK(set->set_hash == set0->set_hash);
        const auto v1_signed{FreshRecord(fb, *set, {{m_vk_a, &m_bls_a}, {m_vk_b, &m_bls_b}, {m_vk_c, &m_bls_c},
                                                    {m_vk_d, &m_bls_d}})};
        BOOST_CHECK_EQUAL(ProbeFresh({}, {v1_signed}), "bad-finality-cert, fresh-bad-signature");
        const auto fresh_signed{FreshRecord(fb, *set, {{m_vk_a, &g_a}, {m_vk_c, &g_c}, {m_vk_d, &g_d}})};
        ProduceFresh({V1CertForNext(cp0)}, {fresh_signed}); // V1's epoch-0 handover too
    }
    BOOST_CHECK(Fresh()->handover_certified);
    BOOST_CHECK_EQUAL(Fresh()->finalized->height, cp0);
    CheckV1AgainstReference();

    // 5. C binds a second key naming 2 (the old-backup shape) above the
    //    pinned checkpoint, so the reorg below stays legal.
    const int c2_height{M + 25};
    ProduceTo(c2_height - 1, m_vk_a);
    {
        const auto bc2{MakeFreshBinding(m_validator_c, m_vk_c, s_c2, 2)};
        Produce(m_vk_a, {}, {SpendTx(CTransaction{tx_two}, {bc2.cell}, {bc2.record}, /*relay=*/false)});
    }
    const uint256 c2_hash{ChainHashAt(c2_height)};

    // 6. Rotation into epoch 1 at start_1 = R_f + E: Set'_2 = {A, D} on new
    //    keys; C void, B sits out (its binding naming 2 predates the window).
    //    The rotation block carries D's binding naming 2: one block late.
    const int start1{R_F + E};
    ProduceTo(start1 - 1, m_vk_a);
    {
        const auto projected{ProjectedNext()};
        BOOST_REQUIRE(projected.fresh);
        BOOST_CHECK_EQUAL(projected.fresh->epoch, 1U);
        BOOST_CHECK(SeatKey(projected.fresh->next, m_vk_a) == Opt(s_a));
    }
    {
        const auto late{MakeFreshBinding(m_validator_d, m_vk_d, d_late, 2)};
        Produce(m_vk_a, {}, {MakeTx(5, {late.cell}, {late.record})});
    }
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, start1);
    const auto rotated{Fresh()};
    const uint256 rotated_tip{Tip()->GetBlockHash()};
    BOOST_REQUIRE(rotated && rotated->next);
    const auto set2{rotated->next};
    BOOST_CHECK_EQUAL(rotated->epoch, 1U);
    BOOST_CHECK(rotated->epoch_starts == (std::vector<int>{R_F, start1}));
    BOOST_CHECK(rotated->previous->set_hash == set0->set_hash);
    BOOST_CHECK(rotated->current->set_hash == set1->set_hash);
    BOOST_CHECK(rotated->current->members == set0->members); // epochs 0 and 1 share the genesis keys
    BOOST_CHECK_EQUAL(set2->Epoch(), 2U);
    BOOST_CHECK_EQUAL(set2->Size(), 2U);
    BOOST_CHECK(SeatKey(set2, m_vk_a) == Opt(s_a));
    BOOST_CHECK(SeatKey(set2, m_vk_d) == Opt(s_d));
    BOOST_CHECK(!SeatKey(set2, m_vk_b));
    BOOST_CHECK(!SeatKey(set2, m_vk_c));
    for (const auto& member : set2->members->Members()) {
        for (const auto* k : {&g_a, &g_b, &g_c, &g_d}) BOOST_CHECK(member.bls_pubkey != Pk(*k));
        for (const auto* k : v1_keys) BOOST_CHECK(member.bls_pubkey != Pk(*k));
    }
    {
        const auto index{FreshKeysAtTip()};
        BOOST_CHECK(node::FreshSeatFor(index, m_vk_c, 2, R_F, start1 - 1).voided);
        BOOST_CHECK(node::FreshSeatFor(index, m_vk_d, 2, R_F, start1 - 1).Key() == Opt(s_d));
        BOOST_CHECK(node::FreshSeatFor(index, m_vk_d, 2, R_F, start1).voided); // the window matters
        BOOST_CHECK(node::FreshSeatFor(index, m_vk_b, 2, R_F, start1 - 1).pubkeys.empty());
        BOOST_CHECK(node::FreshSeatFor(index, m_vk_b, 2, R_KEY, start1 - 1).Key() == Opt(b_early));
        BOOST_CHECK(node::FreshSeatFor(index, m_vk_b, 1, R_KEY, R_F - 1).Key() == Opt(b_one));
    }
    CheckV1AgainstReference();

    // 7. Reorg below C's second binding: a branch without it seats C on its
    //    single key; back on the original branch every committee returns.
    Invalidate(c2_hash);
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, c2_height - 1);
    ProduceTo(start1, m_vk_a);
    {
        const auto alt{Fresh()};
        BOOST_REQUIRE(alt && alt->next);
        BOOST_CHECK_EQUAL(alt->epoch, 1U);
        BOOST_CHECK(alt->current->set_hash == set1->set_hash);
        BOOST_CHECK_EQUAL(alt->next->Size(), 3U);
        BOOST_CHECK(SeatKey(alt->next, m_vk_a) == Opt(s_a));
        BOOST_CHECK(SeatKey(alt->next, m_vk_c) == Opt(s_c1));
        BOOST_CHECK(SeatKey(alt->next, m_vk_d) == Opt(s_d));
        BOOST_CHECK(alt->next->set_hash != set2->set_hash);
    }
    CheckV1AgainstReference();
    Invalidate(ChainHashAt(c2_height));
    Reconsider(c2_hash);
    BOOST_REQUIRE(Tip()->GetBlockHash() == rotated_tip);
    BOOST_CHECK(SameFreshState(Fresh(), rotated));
    CheckV1AgainstReference();

    // 8. V1's epoch-1 handover, then the fresh epoch-1 handover (checkpoint
    //    G + 35) by Set'_1 on the genesis keys; the epoch-2 keys cannot sign it.
    Produce(m_vk_a, {V1CertForNext(M + 30)});
    const int cp1{G + 35};
    ProduceTo(cp1 + SCALED_DEPTH - 1, m_vk_a);
    {
        const auto fb{FreshFbForNext(cp1)};
        BOOST_CHECK_EQUAL(fb.epoch, 1U);
        BOOST_CHECK(fb.validator_set_hash == set2->set_hash);
        const auto set{SigningSetForNext(1)};
        BOOST_CHECK(set->set_hash == set1->set_hash);
        BOOST_CHECK_EQUAL(ProbeFresh({}, {FreshRecord(fb, *set, {{m_vk_a, &s_a}, {m_vk_c, &g_c}, {m_vk_d, &s_d}})}),
                          "bad-finality-cert, fresh-bad-signature");
        ProduceFresh({}, {FreshRecord(fb, *set, {{m_vk_a, &g_a}, {m_vk_b, &g_b}, {m_vk_c, &g_c}, {m_vk_d, &g_d}})});
    }
    BOOST_CHECK(Fresh()->handover_certified);

    // 9. Rotation into epoch 2: nobody named epoch 3 in [start_1, start_2 - 1],
    //    so Set'_3 carries Set'_2 over (same members and keys).
    const int start2{start1 + E};
    ProduceTo(start2, m_vk_a);
    {
        const auto fresh{Fresh()};
        BOOST_REQUIRE(fresh && fresh->next);
        BOOST_CHECK_EQUAL(fresh->epoch, 2U);
        BOOST_CHECK(fresh->current->set_hash == set2->set_hash);
        BOOST_CHECK(fresh->next->members == fresh->current->members);
        BOOST_CHECK_EQUAL(fresh->next->Epoch(), 3U);
    }

    // 10. Epoch 2 is signed on the new keys only: the genesis keys and the V1
    //     keys of A and D are refused, s_a + s_d certify.
    const int cp2{G + 65};
    ProduceTo(cp2 + SCALED_DEPTH - 1, m_vk_a);
    {
        const auto fb{FreshFbForNext(cp2)};
        BOOST_CHECK_EQUAL(fb.epoch, 2U);
        const auto set{SigningSetForNext(2)};
        BOOST_CHECK(set->set_hash == set2->set_hash);
        BOOST_CHECK_EQUAL(ProbeFresh({}, {FreshRecord(fb, *set, {{m_vk_a, &g_a}, {m_vk_d, &g_d}})}),
                          "bad-finality-cert, fresh-bad-signature");
        BOOST_CHECK_EQUAL(ProbeFresh({}, {FreshRecord(fb, *set, {{m_vk_a, &m_bls_a}, {m_vk_d, &m_bls_d}})}),
                          "bad-finality-cert, fresh-bad-signature");
        ProduceFresh({}, {FreshRecord(fb, *set, {{m_vk_a, &s_a}, {m_vk_d, &s_d}})});
    }
    BOOST_CHECK(Fresh()->handover_certified);
    BOOST_CHECK_EQUAL(Fresh()->finalized->height, cp2);
    CheckV1AgainstReference();

    // 11. A full rebuild and a dirty re-sync reproduce every committee.
    const auto live{FinalityState()};
    {
        LOCK(cs_main);
        node::FinalityTracker rebuilt;
        const CChain& chain{m_node.chainman->ActiveChain()};
        BOOST_REQUIRE(rebuilt.Sync(chain, m_node.chainman->m_blockman, consensus, *chain.Tip()));
        BOOST_CHECK(SameFreshState(rebuilt.Current().fresh, live.fresh));
        BOOST_CHECK(SameV1Fields(rebuilt.Current(), live));
    }
    WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().ModernFinality().MarkDirty());
    const auto resynced{FinalityState()};
    BOOST_CHECK(SameFreshState(resynced.fresh, live.fresh));
    BOOST_CHECK(SameV1Fields(resynced, live));
}

// ---------------------------------------------------------------------------
// Store layer (node/finality_fresh_key_store.h): the signer's fresh key store
// outside the wallet, and the erasure policy.

namespace {

using node::FreshKeyRecord;
using node::FreshKeyState;
using node::FreshKeyStore;
using Bytes = std::vector<unsigned char>;

Bytes ReadFileBytes(const fs::path& path)
{
    std::ifstream in{path.std_path(), std::ios::binary};
    BOOST_REQUIRE(in.good());
    return Bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

void WriteFileBytes(const fs::path& path, const Bytes& bytes)
{
    std::ofstream out{path.std_path(), std::ios::binary | std::ios::trunc};
    BOOST_REQUIRE(out.good());
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    BOOST_REQUIRE(out.good());
}

bool Contains(const Bytes& haystack, std::span<const unsigned char> needle)
{
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) != haystack.end();
}

//! Recompute the trailing SHA256 over every preceding byte.
void Rechecksum(Bytes& bytes)
{
    BOOST_REQUIRE(bytes.size() >= FreshKeyStore::CHECKSUM_SIZE);
    const size_t body{bytes.size() - FreshKeyStore::CHECKSUM_SIZE};
    CSHA256().Write(bytes.data(), body).Finalize(bytes.data() + body);
}

struct RawRecord {
    uint64_t epoch{0};
    uint8_t state{1};
    BlsPubkeyBytes pk{};
    std::array<unsigned char, 32> secret{};
    uint256 txid{};
};

//! The documented file layout, written independently of the store.
Bytes StoreFile(const uint256& domain, const ValidatorKeyBytes& vk, const std::vector<RawRecord>& records,
                const std::optional<uint32_t> count_override = std::nullopt)
{
    Bytes out{'B', '3', 'F', 'K', 'E', 'Y', 'S', '1', 0x01};
    out.insert(out.end(), domain.begin(), domain.end());
    out.insert(out.end(), vk.begin(), vk.end());
    unsigned char le[8];
    WriteLE32(le, count_override.value_or(static_cast<uint32_t>(records.size())));
    out.insert(out.end(), le, le + 4);
    for (const RawRecord& r : records) {
        WriteLE64(le, r.epoch);
        out.insert(out.end(), le, le + 8);
        out.push_back(r.state);
        out.insert(out.end(), r.pk.begin(), r.pk.end());
        out.insert(out.end(), r.secret.begin(), r.secret.end());
        out.insert(out.end(), r.txid.begin(), r.txid.end());
    }
    out.resize(out.size() + 32);
    Rechecksum(out);
    return out;
}

RawRecord HeldRecord(const uint64_t epoch, const bls::SecretKey& sk, const uint256& txid = uint256{})
{
    return RawRecord{epoch, 1, Pk(sk), sk.Bytes(), txid};
}

//! Open must fail closed with `expect` in its error and leave the store closed.
void CheckOpenFails(const fs::path& dir, const uint256& domain, const ValidatorKeyBytes& vk, const std::string& expect)
{
    FreshKeyStore store;
    std::string error;
    BOOST_CHECK(!store.Open(dir, domain, vk, error));
    BOOST_CHECK_MESSAGE(error.find(expect) != std::string::npos, "error '" << error << "' lacks '" << expect << "'");
    BOOST_CHECK(!store.IsOpen());
    std::string gen_error;
    BOOST_CHECK(!store.Generate(0, gen_error));
    BOOST_CHECK_EQUAL(gen_error, "fresh key store is not open");
}

//! A committee over (validator, key) pairs, stamped `epoch`.
std::shared_ptr<const node::FreshLineageSet> KeySet(const std::vector<std::pair<const Validator*, BlsPubkeyBytes>>& seats,
                                                    const uint64_t epoch)
{
    node::FinalityBindingIndex index;
    std::vector<node::FinalityBindingIndex::Transition> bound;
    std::map<node::ValidatorKey, CAmount> weights;
    for (const auto& [v, pk] : seats) {
        modern::BindingRecord record;
        record.bls_pubkey = pk;
        record.height = 10;
        bound.push_back({v->vk, record});
        weights[v->vk] = 10 * modern::FINALITY_WEIGHT_UNIT;
    }
    index.ConnectBlock(10, bound);
    const auto snapshot{node::ValidatorSetSnapshot::Build(0, weights, index)};
    BOOST_REQUIRE(snapshot);
    return node::MakeFreshLineageSet(std::make_shared<const node::ValidatorSetSnapshot>(*snapshot), epoch);
}

} // namespace

BOOST_AUTO_TEST_CASE(fresh_key_store_generate_reopen_layout_and_independence)
{
    const fs::path dir{m_path_root / "fresh_store_generate" / node::FINALITY_SIGNER_DIRNAME};
    const Validator a{1}, b{2};
    const fs::path path_a{FreshKeyStore::StatePath(dir, CHAIN_DOMAIN, a.vk)};
    BOOST_CHECK_EQUAL(fs::PathToString(path_a.filename()),
                      "finality_fresh_keys_" + CHAIN_DOMAIN.GetHex() + "_" + HexStr(a.vk) + ".dat");
    BOOST_CHECK(FreshKeyStore::StatePath(dir, OTHER_CHAIN_DOMAIN, a.vk) != path_a);
    BOOST_CHECK(FreshKeyStore::StatePath(dir, CHAIN_DOMAIN, b.vk) != path_a);
    BOOST_CHECK_EQUAL(FreshKeyStore::HEADER_SIZE, 77U);
    BOOST_CHECK_EQUAL(FreshKeyStore::RECORD_SIZE, 121U);
    BOOST_CHECK_EQUAL(FreshKeyStore::MAX_RECORDS, 4096U);
    BOOST_CHECK_EQUAL(std::string{node::FreshKeyStateName(FreshKeyState::HELD)}, "held");
    BOOST_CHECK_EQUAL(std::string{node::FreshKeyStateName(FreshKeyState::ERASED)}, "erased");

    std::string error;
    {
        FreshKeyStore bad;
        BOOST_CHECK(!bad.Open(fs::path{}, CHAIN_DOMAIN, a.vk, error));
        BOOST_CHECK(!bad.Open(dir, uint256{}, a.vk, error));
        BOOST_CHECK(!bad.Open(dir, CHAIN_DOMAIN, ValidatorKeyBytes{}, error));
        BOOST_CHECK_EQUAL(error, "invalid fresh key store identity");
    }

    // An absent store opens empty and creates nothing.
    FreshKeyStore store;
    BOOST_REQUIRE(store.Open(dir, CHAIN_DOMAIN, a.vk, error));
    BOOST_CHECK(store.IsAbsent());
    BOOST_CHECK(store.Records().empty());
    BOOST_CHECK(!fs::exists(dir));
    BOOST_CHECK(!store.HoldsKeyForEpoch(0));

    // Generate: durable, held, returned and retrievable.
    const auto k0{store.Generate(0, error)};
    BOOST_REQUIRE_MESSAGE(k0, error);
    BOOST_CHECK(fs::exists(path_a));
    BOOST_CHECK(!store.IsAbsent());
    BOOST_CHECK(store.HoldsKeyForEpoch(0));
    BOOST_REQUIRE_EQUAL(store.Records().size(), 1U);
    BOOST_CHECK(store.Records()[0] == (FreshKeyRecord{0, FreshKeyState::HELD, Pk(*k0), uint256{}}));
    BOOST_REQUIRE(store.SecretFor(Pk(*k0)));
    BOOST_CHECK(store.SecretFor(Pk(*k0))->Bytes() == k0->Bytes());
    // The exact documented layout, byte for byte.
    BOOST_CHECK(ReadFileBytes(path_a) == StoreFile(CHAIN_DOMAIN, a.vk, {HeldRecord(0, *k0)}));
    BOOST_CHECK_EQUAL(ReadFileBytes(path_a).size(), 77U + 121U + 32U);
#ifndef WIN32
    {
        const auto perms{fs::status(path_a).permissions()};
        BOOST_CHECK((perms & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
        BOOST_CHECK((perms & fs::perms::owner_read) != fs::perms::none);
        BOOST_CHECK((perms & fs::perms::owner_write) != fs::perms::none);
    }
#endif

    // A second HELD key for one epoch is refused (it would void the seat);
    // nothing is written.
    const Bytes before{ReadFileBytes(path_a)};
    BOOST_CHECK(!store.Generate(0, error));
    BOOST_CHECK(error.find("already held") != std::string::npos);
    BOOST_CHECK(ReadFileBytes(path_a) == before);
    BOOST_CHECK_EQUAL(store.Records().size(), 1U);
    // Another epoch gets a new, different key.
    const auto k0b{store.Generate(2, error)};
    BOOST_REQUIRE_MESSAGE(k0b, error);
    const auto k1{store.Generate(1, error)};
    BOOST_REQUIRE_MESSAGE(k1, error);
    const auto k7{store.Generate(7, error)};
    BOOST_REQUIRE_MESSAGE(k7, error);
    const std::set<BlsPubkeyBytes> distinct{Pk(*k0), Pk(*k0b), Pk(*k1), Pk(*k7)};
    BOOST_CHECK_EQUAL(distinct.size(), 4U);
    BOOST_CHECK(ReadFileBytes(path_a) ==
                StoreFile(CHAIN_DOMAIN, a.vk,
                          {HeldRecord(0, *k0), HeldRecord(2, *k0b), HeldRecord(1, *k1), HeldRecord(7, *k7)}));

    // Every key signs and verifies under its public key.
    const uint256 digest{uint256{"5151515151515151515151515151515151515151515151515151515151515151"}};
    for (const auto* sk : {&*k0, &*k0b, &*k1, &*k7}) {
        BOOST_CHECK(bls::Verify(sk->GetPublicKey(), digest, sk->Sign(digest)));
    }

    // Reopen: same records, same secrets.
    {
        FreshKeyStore reopened;
        BOOST_REQUIRE(reopened.Open(dir, CHAIN_DOMAIN, a.vk, error));
        BOOST_CHECK(reopened.Records() == store.Records());
        for (const auto* sk : {&*k0, &*k0b, &*k1, &*k7}) {
            const auto secret{reopened.SecretFor(Pk(*sk))};
            BOOST_REQUIRE(secret);
            BOOST_CHECK(secret->Bytes() == sk->Bytes());
        }
        BOOST_CHECK(!reopened.SecretFor(Pk(BlsKey(99))));
        BOOST_CHECK(!reopened.RecordFor(Pk(BlsKey(99))));
    }

    // Another validator in the same directory: its own file, unrelated keys.
    // Nothing is derived from an identity: the same identity's store on
    // another chain draws new keys too.
    FreshKeyStore other;
    BOOST_REQUIRE(other.Open(dir, CHAIN_DOMAIN, b.vk, error));
    BOOST_CHECK(other.IsAbsent());
    const auto kb{other.Generate(0, error)};
    BOOST_REQUIRE_MESSAGE(kb, error);
    FreshKeyStore other_chain;
    BOOST_REQUIRE(other_chain.Open(dir, OTHER_CHAIN_DOMAIN, a.vk, error));
    const auto ka_other{other_chain.Generate(0, error)};
    BOOST_REQUIRE_MESSAGE(ka_other, error);
    for (const auto* sk : {&*kb, &*ka_other}) {
        BOOST_CHECK(!distinct.contains(Pk(*sk)));
    }
    BOOST_CHECK(Pk(*kb) != Pk(*ka_other));

    // Exactly the three store files; no temporary file is left behind.
    size_t files{0};
    for (const auto& entry : fs::directory_iterator(dir)) {
        ++files;
        BOOST_CHECK(fs::PathToString(entry.path().filename()).starts_with("finality_fresh_keys_"));
        BOOST_CHECK(entry.path().extension() == ".dat");
    }
    BOOST_CHECK_EQUAL(files, 3U);
}

BOOST_AUTO_TEST_CASE(fresh_key_store_bind_txid_and_erase)
{
    const fs::path dir{m_path_root / "fresh_store_erase"};
    const Validator a{1};
    const fs::path path{FreshKeyStore::StatePath(dir, CHAIN_DOMAIN, a.vk)};
    const uint256 T1{uint256{"7171717171717171717171717171717171717171717171717171717171717171"}};
    const uint256 T2{uint256{"7272727272727272727272727272727272727272727272727272727272727272"}};
    std::string error;
    FreshKeyStore store;
    BOOST_REQUIRE(store.Open(dir, CHAIN_DOMAIN, a.vk, error));
    const auto k0{store.Generate(0, error)};
    const auto k1{store.Generate(1, error)};
    BOOST_REQUIRE(k0 && k1);

    // Bind txid: recorded durably, idempotent, never changed.
    BOOST_CHECK(store.SetBindTxid(Pk(*k0), T1, error));
    BOOST_CHECK(store.SetBindTxid(Pk(*k0), T1, error));
    BOOST_CHECK(!store.SetBindTxid(Pk(*k0), T2, error));
    BOOST_CHECK(error.find("never bound twice") != std::string::npos);
    BOOST_CHECK(!store.SetBindTxid(Pk(*k0), uint256{}, error));
    BOOST_CHECK_EQUAL(error, "null binding txid");
    BOOST_CHECK(!store.SetBindTxid(Pk(BlsKey(99)), T2, error));
    BOOST_CHECK_EQUAL(error, "unknown fresh key");
    BOOST_CHECK(store.RecordFor(Pk(*k0))->bind_txid == T1);
    BOOST_CHECK(store.RecordFor(Pk(*k1))->bind_txid.IsNull());
    {
        FreshKeyStore reopened;
        BOOST_REQUIRE(reopened.Open(dir, CHAIN_DOMAIN, a.vk, error));
        BOOST_CHECK(reopened.RecordFor(Pk(*k0))->bind_txid == T1);
    }

    // Before erasure the file holds both secrets.
    const auto s0{k0->Bytes()};
    const auto s1{k1->Bytes()};
    BOOST_CHECK(Contains(ReadFileBytes(path), s0));
    BOOST_CHECK(Contains(ReadFileBytes(path), s1));

    // Erase: the secret is gone from memory and from the file; the public
    // record (key, epoch, bind txid) stays.
    BOOST_CHECK(store.Erase(Pk(*k0), error));
    BOOST_CHECK(!store.SecretFor(Pk(*k0)));
    BOOST_CHECK(!store.HoldsKeyForEpoch(0));
    BOOST_CHECK(store.HoldsKeyForEpoch(1));
    BOOST_CHECK(*store.RecordFor(Pk(*k0)) == (FreshKeyRecord{0, FreshKeyState::ERASED, Pk(*k0), T1}));
    const Bytes erased_file{ReadFileBytes(path)};
    BOOST_CHECK(!Contains(erased_file, s0));
    BOOST_CHECK(Contains(erased_file, s1));
    BOOST_CHECK(erased_file == StoreFile(CHAIN_DOMAIN, a.vk, {RawRecord{0, 2, Pk(*k0), {}, T1}, HeldRecord(1, *k1)}));
    {
        FreshKeyStore reopened;
        BOOST_REQUIRE(reopened.Open(dir, CHAIN_DOMAIN, a.vk, error));
        BOOST_CHECK(!reopened.SecretFor(Pk(*k0)));
        BOOST_CHECK(reopened.RecordFor(Pk(*k0))->state == FreshKeyState::ERASED);
        BOOST_CHECK(reopened.SecretFor(Pk(*k1))->Bytes() == s1);
    }
    // Idempotent; an erased key takes no bind txid; unknown keys refused.
    BOOST_CHECK(store.Erase(Pk(*k0), error));
    BOOST_CHECK(ReadFileBytes(path) == erased_file);
    BOOST_CHECK(!store.SetBindTxid(Pk(*k0), T1, error));
    BOOST_CHECK_EQUAL(error, "fresh key is erased");
    BOOST_CHECK(!store.Erase(Pk(BlsKey(99)), error));
    BOOST_CHECK_EQUAL(error, "unknown fresh key");

    // With no HELD key left for epoch 0, a new one may be generated; it is a
    // new key, never the erased one.
    const auto k0_new{store.Generate(0, error)};
    BOOST_REQUIRE_MESSAGE(k0_new, error);
    BOOST_CHECK(Pk(*k0_new) != Pk(*k0));
    BOOST_CHECK(store.Erase(Pk(*k1), error));
    BOOST_CHECK(store.Erase(Pk(*k0_new), error));
    const Bytes all_erased{ReadFileBytes(path)};
    BOOST_CHECK(!Contains(all_erased, s0));
    BOOST_CHECK(!Contains(all_erased, s1));
    BOOST_CHECK(!Contains(all_erased, k0_new->Bytes()));
}

BOOST_AUTO_TEST_CASE(fresh_key_store_fails_closed)
{
    const fs::path dir{m_path_root / "fresh_store_closed"};
    const Validator a{1}, b{2}, c{3};
    const fs::path path{FreshKeyStore::StatePath(dir, CHAIN_DOMAIN, a.vk)};
    const auto k{BlsKey(41)};
    const Bytes good{StoreFile(CHAIN_DOMAIN, a.vk, {HeldRecord(3, k)})};
    fs::create_directories(dir);
    WriteFileBytes(path, good);
    std::string error;
    {
        FreshKeyStore store;
        BOOST_REQUIRE_MESSAGE(store.Open(dir, CHAIN_DOMAIN, a.vk, error), error);
        BOOST_CHECK(store.SecretFor(Pk(k))->Bytes() == k.Bytes());
    }
    constexpr size_t REC{FreshKeyStore::HEADER_SIZE};
    const auto corrupt{[&](const std::function<void(Bytes&)>& edit, const bool rechecksum, const std::string& expect) {
        Bytes bytes{good};
        edit(bytes);
        if (rechecksum) Rechecksum(bytes);
        WriteFileBytes(path, bytes);
        CheckOpenFails(dir, CHAIN_DOMAIN, a.vk, expect);
    }};
    corrupt([](Bytes& x) { x[0] ^= 1; }, true, "magic");
    corrupt([](Bytes& x) { x[8] = 2; }, true, "version 2");
    corrupt([](Bytes& x) { x[REC + 20] ^= 1; }, false, "checksum mismatch");
    corrupt([](Bytes& x) { x.back() ^= 1; }, false, "checksum mismatch");
    corrupt([](Bytes& x) { x.push_back(0); }, false, "checksum mismatch");
    corrupt([](Bytes& x) { x.push_back(0); }, true, "length does not match");
    corrupt([](Bytes& x) { x.resize(40); }, false, "truncated");
    corrupt([](Bytes& x) { x.clear(); }, false, "truncated");
    corrupt([](Bytes& x) { WriteLE32(x.data() + 73, 2); }, true, "length does not match");
    corrupt([](Bytes& x) { x[REC + 8] = 3; }, true, "unknown state 3");
    corrupt([](Bytes& x) { x[REC + 8] = 0; }, true, "unknown state 0");
    corrupt([](Bytes& x) { x[REC + 9 + 48 + 31] ^= 1; }, true, "does not match its public key");
    corrupt([](Bytes& x) { std::fill_n(x.begin() + REC + 9 + 48, 32, 0); }, true, "does not match its public key");
    corrupt([](Bytes& x) { x[REC + 8] = 2; }, true, "still holds secret bytes");
    corrupt([](Bytes& x) { std::fill_n(x.begin() + REC + 9, 48, 0xff); }, true, "non-canonical");
    corrupt([](Bytes& x) { std::fill_n(x.begin() + 9, 32, 0); }, true, "null identity");
    {
        // A duplicate public key (two records of one key).
        const Bytes dup{StoreFile(CHAIN_DOMAIN, a.vk, {HeldRecord(3, k), RawRecord{4, 2, Pk(k), {}, {}}})};
        WriteFileBytes(path, dup);
        CheckOpenFails(dir, CHAIN_DOMAIN, a.vk, "repeats a BLS public key");
    }

    // A valid file of another chain or validator, copied under this name.
    WriteFileBytes(path, good);
    const fs::path foreign_domain{FreshKeyStore::StatePath(dir, OTHER_CHAIN_DOMAIN, a.vk)};
    const fs::path foreign_validator{FreshKeyStore::StatePath(dir, CHAIN_DOMAIN, b.vk)};
    WriteFileBytes(foreign_domain, good);
    WriteFileBytes(foreign_validator, good);
    CheckOpenFails(dir, OTHER_CHAIN_DOMAIN, a.vk, "belongs to another chain or validator");
    CheckOpenFails(dir, CHAIN_DOMAIN, b.vk, "belongs to another chain or validator");

    // A directory where the file belongs is not an empty store.
    fs::create_directories(FreshKeyStore::StatePath(dir, CHAIN_DOMAIN, c.vk));
    CheckOpenFails(dir, CHAIN_DOMAIN, c.vk, "not a regular file");

    // A mutation never replaces an unsafe, changed or vanished predecessor.
    FreshKeyStore s1;
    FreshKeyStore s2;
    BOOST_REQUIRE(s1.Open(dir, CHAIN_DOMAIN, a.vk, error));
    BOOST_REQUIRE(s2.Open(dir, CHAIN_DOMAIN, a.vk, error));
    {
        Bytes bad{good};
        bad[REC + 20] ^= 1;
        WriteFileBytes(path, bad);
        BOOST_CHECK(!s1.Generate(4, error));
        BOOST_CHECK(error.find("refusing to replace an unsafe fresh key store") != std::string::npos);
        BOOST_CHECK(!s1.Erase(Pk(k), error));
        BOOST_CHECK(ReadFileBytes(path) == bad);
        WriteFileBytes(path, good);
    }
    const auto k4{s1.Generate(4, error)};
    BOOST_REQUIRE_MESSAGE(k4, error);
    BOOST_CHECK(!s2.Generate(5, error));
    BOOST_CHECK_EQUAL(error, "fresh key store changed or disappeared on disk; reopen it");
    BOOST_CHECK(!s2.Erase(Pk(k), error));
    BOOST_REQUIRE(s2.Open(dir, CHAIN_DOMAIN, a.vk, error));
    BOOST_CHECK(s2.SecretFor(Pk(*k4))->Bytes() == k4->Bytes());
    BOOST_CHECK(s2.Generate(5, error));
    fs::remove(path);
    BOOST_CHECK(!s2.Erase(Pk(k), error));
    BOOST_CHECK_EQUAL(error, "fresh key store changed or disappeared on disk; reopen it");
    BOOST_CHECK(!fs::exists(path));

    // A store opened absent never overwrites a file created meanwhile.
    const Validator d{4};
    FreshKeyStore absent1;
    FreshKeyStore absent2;
    BOOST_REQUIRE(absent1.Open(dir, CHAIN_DOMAIN, d.vk, error));
    BOOST_REQUIRE(absent2.Open(dir, CHAIN_DOMAIN, d.vk, error));
    const auto kd{absent2.Generate(0, error)};
    BOOST_REQUIRE(kd);
    BOOST_CHECK(!absent1.Generate(0, error));
    BOOST_CHECK_EQUAL(error, "fresh key store file appeared after it was opened; reopen it");
    FreshKeyStore check;
    BOOST_REQUIRE(check.Open(dir, CHAIN_DOMAIN, d.vk, error));
    BOOST_REQUIRE_EQUAL(check.Records().size(), 1U);
    BOOST_CHECK(check.Records()[0].bls_pubkey == Pk(*kd));
}

BOOST_AUTO_TEST_CASE(fresh_key_store_max_records)
{
    const fs::path dir{m_path_root / "fresh_store_full"};
    const Validator a{1};
    const fs::path path{FreshKeyStore::StatePath(dir, CHAIN_DOMAIN, a.vk)};
    fs::create_directories(dir);
    // MAX_RECORDS erased records of distinct keys: valid, but full.
    std::vector<RawRecord> records;
    records.reserve(FreshKeyStore::MAX_RECORDS);
    for (size_t i{0}; i < FreshKeyStore::MAX_RECORDS; ++i) {
        std::array<unsigned char, 32> ikm{};
        WriteLE32(ikm.data(), static_cast<uint32_t>(i));
        ikm[31] = 0x8f;
        records.push_back(RawRecord{i, 2, Pk(*bls::SecretKey::FromIKM(ikm)), {}, {}});
    }
    WriteFileBytes(path, StoreFile(CHAIN_DOMAIN, a.vk, records));
    std::string error;
    FreshKeyStore store;
    BOOST_REQUIRE_MESSAGE(store.Open(dir, CHAIN_DOMAIN, a.vk, error), error);
    BOOST_CHECK_EQUAL(store.Records().size(), FreshKeyStore::MAX_RECORDS);
    const Bytes before{ReadFileBytes(path)};
    BOOST_CHECK(!store.Generate(FreshKeyStore::MAX_RECORDS, error));
    BOOST_CHECK(error.find("full") != std::string::npos);
    BOOST_CHECK(ReadFileBytes(path) == before);
    // One record more than MAX_RECORDS is refused before it is even read
    // (the file exceeds the largest valid size).
    records.push_back(records.back());
    WriteFileBytes(path, StoreFile(CHAIN_DOMAIN, a.vk, records));
    CheckOpenFails(dir, CHAIN_DOMAIN, a.vk, "too large");
    // A count above the maximum is refused even with a short body.
    WriteFileBytes(path, StoreFile(CHAIN_DOMAIN, a.vk, {}, FreshKeyStore::MAX_RECORDS + 1));
    CheckOpenFails(dir, CHAIN_DOMAIN, a.vk, "maximum 4096");
}

BOOST_AUTO_TEST_CASE(fresh_key_erasure_policy_truth_table)
{
    constexpr int H{40}; // horizon
    const Validator a{1}, b{2}, c{3}, d{4};
    const auto g_a{BlsKey(101)}, g_b{BlsKey(102)}, g_c{BlsKey(103)}, g_d{BlsKey(104)}; // genesis (target 0)
    const auto k2_a{BlsKey(121)}, k2_b{BlsKey(122)}, k2_c{BlsKey(123)}, k2_d{BlsKey(124)};
    const auto k3_a{BlsKey(131)}, k3_b{BlsKey(132)}, k3_c{BlsKey(133)}, k3_d{BlsKey(134)}, k3_d2{BlsKey(135)};
    const auto k4_a{BlsKey(141)};
    const auto set0{KeySet({{&a, Pk(g_a)}, {&b, Pk(g_b)}, {&c, Pk(g_c)}, {&d, Pk(g_d)}}, 0)};
    const auto set1{node::MakeFreshLineageSet(set0->members, 1)}; // genesis keys serve epochs 0 and 1
    // D's two epoch-2 bindings voided its seat; D also double-bound epoch 3.
    const auto set2{KeySet({{&a, Pk(k2_a)}, {&b, Pk(k2_b)}, {&c, Pk(k2_c)}}, 2)};
    const auto set3{KeySet({{&a, Pk(k3_a)}, {&b, Pk(k3_b)}, {&c, Pk(k3_c)}}, 3)};
    constexpr int S0{262}, S1{292}, S2{322};

    node::FreshLineageState e1;
    e1.bootstrapped = true;
    e1.epoch = 1;
    e1.epoch_starts = {S0, S1};
    e1.genesis = set0;
    e1.previous = set0;
    e1.current = set1;
    e1.next = set2;
    node::FreshLineageState e2{e1};
    e2.epoch = 2;
    e2.epoch_starts = {S0, S1, S2};
    e2.previous2 = set0;
    e2.previous = set1;
    e2.current = set2;
    e2.next = set3;

    const auto erasable{[&](const node::FreshLineageState& s, const bls::SecretKey& sk, const uint64_t target,
                            const int tip, const std::optional<int> horizon = std::nullopt) {
        return node::FreshKeyErasable(s, Pk(sk), target, tip, horizon.value_or(H));
    }};
    constexpr int FAR{1'000'000};

    // Unbootstrapped: keep everything.
    BOOST_CHECK(!erasable(node::FreshLineageState{}, g_a, 0, FAR));
    {
        node::FreshLineageState unboot{e2};
        unboot.bootstrapped = false;
        BOOST_CHECK(!erasable(unboot, g_a, 0, FAR));
    }

    // Epoch 1: the genesis keys are the current committee's; the epoch-2
    // keys are the next committee's. Keep.
    BOOST_CHECK(!erasable(e1, g_a, 0, FAR));
    BOOST_CHECK(!erasable(e1, g_d, 0, FAR));
    BOOST_CHECK(!erasable(e1, k2_a, 2, FAR));
    // D's voided epoch-2 key (target e+1, not seated): its committee is
    // fixed, so it goes at start_1 + H.
    BOOST_CHECK(!erasable(e1, k2_d, 2, S1 + H - 1));
    BOOST_CHECK(erasable(e1, k2_d, 2, S1 + H));
    // A key for a later epoch (target e+2) is kept: its committee is open.
    BOOST_CHECK(!erasable(e1, k3_a, 3, FAR));

    // Epoch 2: the genesis keys (last seated in epoch 1) go once the
    // handover into epoch 2 is H deep: at start_2 + H, not one block earlier.
    for (const auto* g : {&g_a, &g_b, &g_c, &g_d}) {
        BOOST_CHECK(!erasable(e2, *g, 0, S2));
        BOOST_CHECK(!erasable(e2, *g, 0, S2 + H - 1));
        BOOST_CHECK(erasable(e2, *g, 0, S2 + H));
        BOOST_CHECK(erasable(e2, *g, 0, FAR));
    }
    // Current (Set'_2) and next (Set'_3) keys stay.
    for (const auto* k : {&k2_a, &k2_b, &k2_c}) BOOST_CHECK(!erasable(e2, *k, 2, FAR));
    for (const auto* k : {&k3_a, &k3_b, &k3_c}) BOOST_CHECK(!erasable(e2, *k, 3, FAR));
    // Never seated, target e (void): Set'_2 was fixed at start_1, so the key
    // stays erasable from start_1 + H as it was in epoch 1 (not only from
    // start_2 + H). Target e+1 (void): Set'_3 was fixed at start_2, so from
    // start_2 + H.
    BOOST_CHECK_EQUAL(node::FreshKeyRetiredSince(e2, Pk(k2_d), 2).value_or(-1), S1);
    BOOST_CHECK(!erasable(e2, k2_d, 2, S1 + H - 1));
    BOOST_CHECK(erasable(e2, k2_d, 2, S1 + H));
    BOOST_CHECK(erasable(e2, k2_d, 2, S2 + H));
    BOOST_CHECK_EQUAL(node::FreshKeyRetiredSince(e2, Pk(k3_d), 3).value_or(-1), S2);
    for (const auto* k : {&k3_d, &k3_d2}) {
        BOOST_CHECK(!erasable(e2, *k, 3, S2 + H - 1));
        BOOST_CHECK(erasable(e2, *k, 3, S2 + H));
    }
    // Future target (e+2) keep, however deep.
    BOOST_CHECK(!erasable(e2, k4_a, 4, FAR));
    BOOST_CHECK(!erasable(e2, k4_a, std::numeric_limits<uint64_t>::max(), FAR));

    // Carry-over: Set'_2 below the floor carried Set'_1 (the genesis keys)
    // into epoch 2, and Set'_3 carries them again. They stay seated, so they
    // are kept; the unused epoch-2 bindings go.
    {
        node::FreshLineageState carried{e2};
        carried.current = node::MakeFreshLineageSet(set1->members, 2);
        carried.next = node::MakeFreshLineageSet(set1->members, 3);
        for (const auto* g : {&g_a, &g_b, &g_c, &g_d}) BOOST_CHECK(!erasable(carried, *g, 0, FAR));
        // The carry-over that skipped them was decided at start_1.
        BOOST_CHECK(erasable(carried, k2_a, 2, S1 + H));
        BOOST_CHECK(!erasable(carried, k2_a, 2, S1 + H - 1));
        // Only the next committee still carries them: still kept.
        carried.current = set2;
        for (const auto* g : {&g_a, &g_b, &g_c, &g_d}) BOOST_CHECK(!erasable(carried, *g, 0, FAR));
    }

    // Degenerate inputs keep: no horizon, a malformed state, a tip below
    // the epoch start.
    BOOST_CHECK(!erasable(e2, g_a, 0, FAR, 0));
    BOOST_CHECK(!erasable(e2, g_a, 0, FAR, -1));
    BOOST_CHECK(!erasable(e2, g_a, 0, std::numeric_limits<int>::max(), std::numeric_limits<int>::max()));
    BOOST_CHECK(erasable(e2, g_a, 0, std::numeric_limits<int>::max(), std::numeric_limits<int>::max() - S2));
    BOOST_CHECK(!erasable(e2, g_a, 0, S2 - 1, 1));
    {
        node::FreshLineageState short_starts{e2};
        short_starts.epoch_starts.pop_back();
        BOOST_CHECK(!erasable(short_starts, g_a, 0, FAR));
        node::FreshLineageState no_next{e2};
        no_next.next.reset();
        BOOST_CHECK(!erasable(no_next, g_a, 0, FAR));
        // Without a readable Set'_{e-1} the depth counts from start_e (later).
        node::FreshLineageState no_previous{e2};
        no_previous.previous.reset();
        BOOST_CHECK_EQUAL(node::FreshKeyRetiredSince(no_previous, Pk(k2_d), 2).value_or(-1), S2);
        BOOST_CHECK(!erasable(no_previous, k2_d, 2, S2 + H - 1));
        BOOST_CHECK(erasable(no_previous, k2_d, 2, S2 + H));
    }

    // A timely chain whose horizon equals E (mainnet: E = horizon = 1,440):
    // every epoch lasts exactly E blocks, so a depth counted from the
    // current epoch's start would never reach the horizon. The genesis keys
    // (retired at start_2) go at start_2 + E = start_3, as epoch 3 begins;
    // Set'_2's keys (retired at start_3) at start_4, as epoch 4 begins.
    {
        constexpr int E{S1 - S0};
        constexpr int S3{S2 + E}, S4{S3 + E};
        const auto k4_b{BlsKey(142)}, k4_c{BlsKey(143)};
        const auto set4{KeySet({{&a, Pk(k4_a)}, {&b, Pk(k4_b)}, {&c, Pk(k4_c)}}, 4)};
        node::FreshLineageState e3{e2};
        e3.epoch = 3;
        e3.epoch_starts = {S0, S1, S2, S3};
        e3.previous2 = set1;
        e3.previous = set2;
        e3.current = set3;
        e3.next = set4;
        for (const auto* g : {&g_a, &g_b, &g_c, &g_d}) {
            BOOST_CHECK_EQUAL(node::FreshKeyRetiredSince(e3, Pk(*g), 0).value_or(-1), S2);
            BOOST_CHECK(!erasable(e2, *g, 0, S3 - 1, E));
            BOOST_CHECK(erasable(e3, *g, 0, S3, E));
        }
        for (const auto* k : {&k2_a, &k2_b, &k2_c}) {
            BOOST_CHECK_EQUAL(node::FreshKeyRetiredSince(e3, Pk(*k), 2).value_or(-1), S3);
            BOOST_CHECK(!erasable(e3, *k, 2, S4 - 1, E));
        }
        node::FreshLineageState e4{e3};
        e4.epoch = 4;
        e4.epoch_starts.push_back(S4);
        e4.previous2 = set2;
        e4.previous = set3;
        e4.current = set4;
        e4.next = node::MakeFreshLineageSet(set4->members, 5); // carried over
        for (const auto* k : {&k2_a, &k2_b, &k2_c}) BOOST_CHECK(erasable(e4, *k, 2, S4, E));
        // Set'_3's keys were retired at start_4: one epoch later.
        for (const auto* k : {&k3_a, &k3_b, &k3_c}) {
            BOOST_CHECK(!erasable(e4, *k, 3, S4 + E - 1, E));
            BOOST_CHECK(erasable(e4, *k, 3, S4 + E, E));
        }
        // The carried-over Set'_4 keys stay.
        for (const auto* k : {&k4_a, &k4_b, &k4_c}) BOOST_CHECK(!erasable(e4, *k, 4, FAR, E));
    }

    // The horizon: the modern reorg horizon, else E; never without modern PoS.
    Consensus::Params params{Params().GetConsensus()};
    params.modern_pos = Consensus::ModernPosParams{};
    params.modern_pos->finality_epoch_blocks = 30;
    params.modern_pos->reorg_horizon = 40;
    BOOST_CHECK_EQUAL(node::FreshKeyErasureHorizon(params), 40);
    params.modern_pos->reorg_horizon.reset();
    BOOST_CHECK_EQUAL(node::FreshKeyErasureHorizon(params), 30);
    params.modern_pos.reset();
    BOOST_CHECK_EQUAL(node::FreshKeyErasureHorizon(params), std::numeric_limits<int>::max());
    BOOST_CHECK_EQUAL(node::FreshKeyErasureHorizon(CreateChainParams(ArgsManager{}, ChainType::MAIN)->GetConsensus()),
                      1440);
}

// ---------------------------------------------------------------------------
// Signer layer (node/finality_signature.h, fresh-key mode): the seat windows,
// the signer's key status, and the staging signers on a chain.

BOOST_AUTO_TEST_CASE(fresh_journal_lineage_catch_up)
{
    // B-37: a member that sat out fresh epoch 2 (void seat) still holds its
    // epoch-1 lock (Set'_1 -> Set'_2). Its epoch-3 vote does not follow that
    // lock until the lock moves to an included certificate of a later epoch
    // (CommitLineageCatchUp, fresh-lineage journal only).
    const fs::path dir{m_path_root / "fresh_journal_catch_up"};
    const uint256 lineage{m_rng.rand256()};
    const modern::ValidatorKeyBytes validator{Validator{7}.vk};
    const uint256 set1{m_rng.rand256()}, set2{m_rng.rand256()}, set3{m_rng.rand256()}, set4{m_rng.rand256()};
    std::string error;

    node::FinalitySignerStore fresh;
    BOOST_REQUIRE_MESSAGE(fresh.Open(dir, lineage, validator, error, node::FinalitySignerNamespace::FRESH_LINEAGE_V2),
                          error);
    BOOST_REQUIRE_MESSAGE(fresh.InitializeEmpty(error), error);
    // Nothing to catch up from before the first vote.
    BOOST_CHECK(!fresh.CommitLineageCatchUp(381, m_rng.rand256(), m_rng.rand256(), 2, set2, set3, error));
    BOOST_CHECK(error.find("no durable ancestry lock") != std::string::npos);
    const uint256 voted_block{m_rng.rand256()}, voted_digest{m_rng.rand256()};
    BOOST_REQUIRE_MESSAGE(fresh.CommitSignedCheckpoint(336, voted_block, voted_digest, 1, set1, set2, error), error);
    // The epoch-3 vote does not follow the epoch-1 lock.
    BOOST_CHECK(!fresh.CommitSignedCheckpoint(386, m_rng.rand256(), m_rng.rand256(), 3, set3, set4, error));
    BOOST_CHECK(error.find("does not follow the durable validator-set lineage") != std::string::npos);
    // The catch-up needs a strictly later epoch and a strictly higher height.
    BOOST_CHECK(!fresh.CommitLineageCatchUp(341, m_rng.rand256(), m_rng.rand256(), 1, set1, set2, error));
    BOOST_CHECK(!fresh.CommitLineageCatchUp(336, m_rng.rand256(), m_rng.rand256(), 2, set2, set3, error));
    BOOST_CHECK(!fresh.CommitLineageCatchUp(331, m_rng.rand256(), m_rng.rand256(), 2, set2, set3, error));
    BOOST_CHECK(!fresh.CommitLineageCatchUp(381, uint256{}, m_rng.rand256(), 2, set2, set3, error));
    BOOST_CHECK_EQUAL(fresh.State()->lock_height, 336);
    // The included epoch-2 handover certificate (Set'_2 -> Set'_3).
    const uint256 certified_block{m_rng.rand256()}, certified_digest{m_rng.rand256()};
    BOOST_REQUIRE_MESSAGE(fresh.CommitLineageCatchUp(381, certified_block, certified_digest, 2, set2, set3, error),
                          error);
    {
        const node::FinalitySignerState& state{*fresh.State()};
        BOOST_CHECK_EQUAL(state.last_signed_height, 336); // the last actual vote is retained
        BOOST_CHECK(state.last_signed_block_hash == voted_block);
        BOOST_CHECK(state.last_signed_digest == voted_digest);
        BOOST_CHECK_EQUAL(state.lock_height, 381);
        BOOST_CHECK(state.lock_block_hash == certified_block);
        BOOST_CHECK(state.lock_digest == certified_digest);
        BOOST_CHECK_EQUAL(state.lock_epoch, 2U);
        BOOST_CHECK(state.lock_signing_set_hash == set2);
        BOOST_CHECK(state.lock_successor_set_hash == set3);
    }
    // Nothing at or below the new lock; the epoch-3 vote now follows.
    BOOST_CHECK(!fresh.CommitSignedCheckpoint(381, m_rng.rand256(), m_rng.rand256(), 3, set3, set4, error));
    BOOST_REQUIRE_MESSAGE(fresh.CommitSignedCheckpoint(386, m_rng.rand256(), m_rng.rand256(), 3, set3, set4, error),
                          error);
    // Durable across a reopen.
    node::FinalitySignerStore reopened;
    BOOST_REQUIRE_MESSAGE(reopened.Open(dir, lineage, validator, error,
                                        node::FinalitySignerNamespace::FRESH_LINEAGE_V2),
                          error);
    BOOST_CHECK(reopened.State() == fresh.State());

    // A V1 journal keeps V1's exact-lineage rule: no catch-up.
    node::FinalitySignerStore v1;
    BOOST_REQUIRE_MESSAGE(v1.Open(m_path_root / "v1_journal_no_catch_up", lineage, validator, error), error);
    BOOST_REQUIRE_MESSAGE(v1.InitializeEmpty(error), error);
    BOOST_REQUIRE_MESSAGE(v1.CommitSignedCheckpoint(336, voted_block, voted_digest, 1, set1, set2, error), error);
    const auto v1_before{v1.State()};
    BOOST_CHECK(!v1.CommitLineageCatchUp(381, certified_block, certified_digest, 2, set2, set3, error));
    BOOST_CHECK(error.find("fresh-lineage journal only") != std::string::npos);
    BOOST_CHECK(v1.State() == v1_before);
}

BOOST_AUTO_TEST_CASE(fresh_signer_seat_windows_and_key_status)
{
    const Configs cfg;
    const Consensus::Params& p{cfg.keyed};
    constexpr int R_F{Configs::R_F};
    constexpr int R_KEY{Configs::R_KEY};
    constexpr int G{R_F - 1};
    constexpr int S1{R_F + 30}, S2{R_F + 60}, S3{R_F + 90}, S4{R_F + 120};
    constexpr auto npos{std::string::npos};
    using Window = std::optional<std::pair<int, int>>;
    using Entry = node::FreshSignerKeyEntry;
    using Missing = node::FreshMissingSeat;
    const auto window{[](const int lo, const int hi) { return Window{std::make_pair(lo, hi)}; }};
    const Validator a{1}, b{2}, c{3};
    const auto g_a{BlsKey(201)}, g_b{BlsKey(202)}, g_c{BlsKey(203)}, g_c2{BlsKey(204)};
    const auto k2_a{BlsKey(221)}, k2_b{BlsKey(222)}, x2_b{BlsKey(223)}, k3_a{BlsKey(231)}, unbound{BlsKey(240)};

    // Committees consistent with the bindings below (floor 2): C's two
    // genesis bindings void its genesis seat; B's two bindings naming 2 void
    // its epoch-2 seat, so Set'_2 seats A alone; A alone naming 3 is below
    // the floor, so Set'_3 carries Set'_2 over.
    const auto set0{KeySet({{&a, Pk(g_a)}, {&b, Pk(g_b)}}, 0)};
    const auto set1{node::MakeFreshLineageSet(set0->members, 1)};
    const auto set2{KeySet({{&a, Pk(k2_a)}}, 2)};
    const auto set3{node::MakeFreshLineageSet(set2->members, 3)};
    node::FreshLineageState e0;
    e0.bootstrapped = true;
    e0.epoch = 0;
    e0.epoch_starts = {R_F};
    e0.genesis = set0;
    e0.current = set0;
    e0.next = set1;
    node::FreshLineageState e1{e0};
    e1.epoch = 1;
    e1.epoch_starts = {R_F, S1};
    e1.previous = set0;
    e1.current = set1;
    e1.next = set2;
    node::FreshLineageState e2{e1};
    e2.epoch = 2;
    e2.epoch_starts = {R_F, S1, S2};
    e2.previous2 = set0;
    e2.previous = set1;
    e2.current = set2;
    e2.next = set3;
    node::FreshLineageState e4{e2};
    e4.epoch = 4;
    e4.epoch_starts = {R_F, S1, S2, S3, S4};
    e4.previous2 = node::MakeFreshLineageSet(set2->members, 2);
    e4.previous = node::MakeFreshLineageSet(set2->members, 3);
    e4.current = node::MakeFreshLineageSet(set2->members, 4);
    e4.next = node::MakeFreshLineageSet(set2->members, 5);

    // FreshSeatWindow: the windows ProjectFreshLineage applies, with an open
    // window ending at the tip.
    BOOST_CHECK(!node::FreshSeatWindow(0, cfg.production, nullptr, 300)); // no fresh lineage
    BOOST_CHECK(!node::FreshSeatWindow(0, cfg.fresh, nullptr, 300));      // fresh lineage on V1 keys
    BOOST_CHECK(!node::FreshSeatWindow(0, p, nullptr, R_KEY - 1));
    BOOST_CHECK(node::FreshSeatWindow(0, p, nullptr, R_KEY) == window(R_KEY, R_KEY));
    BOOST_CHECK(node::FreshSeatWindow(0, p, nullptr, 250) == window(R_KEY, 250));
    BOOST_CHECK(node::FreshSeatWindow(0, p, &e2, 1000) == window(R_KEY, G));
    for (const node::FreshLineageState* s : std::initializer_list<const node::FreshLineageState*>{nullptr, &e0, &e1, &e2}) {
        BOOST_CHECK(!node::FreshSeatWindow(1, p, s, 1000)); // a binding naming 1 never counts
    }
    BOOST_CHECK(!node::FreshSeatWindow(2, p, nullptr, 300));
    {
        node::FreshLineageState unboot{e1};
        unboot.bootstrapped = false;
        BOOST_CHECK(!node::FreshSeatWindow(2, p, &unboot, 300));
    }
    BOOST_CHECK(!node::FreshSeatWindow(2, p, &e0, G)); // the projection at R_f - 1: not open yet
    BOOST_CHECK(node::FreshSeatWindow(2, p, &e0, R_F) == window(R_F, R_F));
    BOOST_CHECK(node::FreshSeatWindow(2, p, &e0, 270) == window(R_F, 270));
    BOOST_CHECK(!node::FreshSeatWindow(3, p, &e0, 270));
    BOOST_CHECK(node::FreshSeatWindow(2, p, &e1, S1 + 5) == window(R_F, S1 - 1));
    BOOST_CHECK(node::FreshSeatWindow(3, p, &e1, S1 + 5) == window(S1, S1 + 5));
    BOOST_CHECK(!node::FreshSeatWindow(4, p, &e1, S1 + 5));
    BOOST_CHECK(node::FreshSeatWindow(3, p, &e2, S2 + 5) == window(S1, S2 - 1));
    BOOST_CHECK(node::FreshSeatWindow(4, p, &e2, S2 + 5) == window(S2, S2 + 5));

    node::FreshKeyBindingIndex index;
    index.ConnectBlock(245, {NewClaim(a, Pk(g_a), 0), NewClaim(b, Pk(g_b), 0), NewClaim(c, Pk(g_c), 0)});
    index.ConnectBlock(250, {NewClaim(c, Pk(g_c2), 0)});
    index.ConnectBlock(270, {NewClaim(a, Pk(k2_a), 2), NewClaim(b, Pk(k2_b), 2)});
    index.ConnectBlock(280, {NewClaim(b, Pk(x2_b), 2)});
    index.ConnectBlock(S1 + 3, {NewClaim(a, Pk(k3_a), 3)});
    const auto status{[&](const Validator& v, std::vector<Entry> keys, const node::FreshLineageState* s,
                          const int tip) {
        for (Entry& key : keys) key.seated_epochs = {99}; // recomputed, never trusted
        return node::ComputeFreshSignerKeyStatus(v.vk, std::move(keys), &index, s, p, tip);
    }};
    const auto has{[](const std::string& text, const std::string& part) { return text.find(part) != npos; }};

    // Before the lineage: C's genesis seat is void (two counted bindings in
    // [R_key, tip]) and one of them is a key C's signer never held.
    {
        const auto st{status(c, {{0, Pk(g_c), true, {}}}, nullptr, 255)};
        BOOST_CHECK(st.fresh_keys == (std::vector<Entry>{{0, Pk(g_c), true, {}}}));
        BOOST_CHECK(st.missing_seats.empty());
        BOOST_REQUIRE_EQUAL(st.alerts.size(), 2U);
        BOOST_CHECK(has(st.alerts[0], "fresh key " + HexStr(Pk(g_c2)) + " bound for fresh epoch 0 at height 250"));
        BOOST_CHECK(has(st.alerts[0], "is not in this signer's fresh key store"));
        BOOST_CHECK(has(st.alerts[1], "fresh-lineage seat for fresh epoch 0 is void: 2 fresh bindings"));
        BOOST_CHECK(has(st.alerts[1], HexStr(Pk(g_c))) && has(st.alerts[1], HexStr(Pk(g_c2))));
        // Without binding facts nothing is alerted.
        const auto bare{node::ComputeFreshSignerKeyStatus(c.vk, {{0, Pk(g_c), true, {}}}, nullptr, nullptr, p, 255)};
        BOOST_CHECK(bare.alerts.empty());
        BOOST_CHECK(bare.missing_seats.empty());
    }

    // Epoch 1: A holds every key; seats per retained committee.
    {
        const auto st{status(a, {{0, Pk(g_a), true, {}}, {2, Pk(k2_a), true, {}}, {3, Pk(k3_a), true, {}}}, &e1,
                             S1 + 5)};
        BOOST_CHECK(st.fresh_keys == (std::vector<Entry>{{0, Pk(g_a), true, {0, 1}},
                                                          {2, Pk(k2_a), true, {2}},
                                                          {3, Pk(k3_a), true, {}}}));
        BOOST_CHECK(st.missing_seats.empty());
        BOOST_CHECK(st.alerts.empty());
    }
    // A store that lost the epoch-2 key: the next seat is missing, and the
    // chain binding of a key the signer does not have is alerted.
    {
        const auto st{status(a, {{0, Pk(g_a), true, {}}, {3, Pk(k3_a), true, {}}}, &e1, S1 + 5)};
        BOOST_CHECK(st.missing_seats == (std::vector<Missing>{{2, Pk(k2_a)}}));
        BOOST_REQUIRE_EQUAL(st.alerts.size(), 1U);
        BOOST_CHECK(has(st.alerts[0], "fresh key " + HexStr(Pk(k2_a)) + " bound for fresh epoch 2 at height 270"));
    }
    // An erased key: not missing for Set'_{e-1}, missing for Set'_e.
    {
        const auto st{status(a, {{0, Pk(g_a), false, {}}, {2, Pk(k2_a), true, {}}}, &e1, S1 + 5)};
        BOOST_CHECK(st.missing_seats == (std::vector<Missing>{{1, Pk(g_a)}}));
    }
    // B: its epoch-2 seat is void (its own key and an unknown one).
    {
        const auto st{status(b, {{0, Pk(g_b), true, {}}, {2, Pk(k2_b), true, {}}}, &e1, S1 + 5)};
        BOOST_CHECK(st.fresh_keys == (std::vector<Entry>{{0, Pk(g_b), true, {0, 1}}, {2, Pk(k2_b), true, {}}}));
        BOOST_CHECK(st.missing_seats.empty());
        BOOST_REQUIRE_EQUAL(st.alerts.size(), 2U);
        BOOST_CHECK(has(st.alerts[0], "fresh key " + HexStr(Pk(x2_b)) + " bound for fresh epoch 2 at height 280"));
        BOOST_CHECK(has(st.alerts[1], "fresh-lineage seat for fresh epoch 2 is void: 2 fresh bindings"));
        BOOST_CHECK(has(st.alerts[1], HexStr(Pk(k2_b))) && has(st.alerts[1], HexStr(Pk(x2_b))));
    }
    // Offline keys: the epoch comes from the key's chain binding.
    {
        const auto st{status(a, {{std::nullopt, Pk(g_a), true, {}}, {std::nullopt, Pk(unbound), true, {}}}, &e1,
                             S1 + 5)};
        BOOST_CHECK(st.fresh_keys == (std::vector<Entry>{{0, Pk(g_a), true, {0, 1}},
                                                          {std::nullopt, Pk(unbound), true, {}}}));
        BOOST_CHECK(st.missing_seats == (std::vector<Missing>{{2, Pk(k2_a)}}));
        BOOST_CHECK_EQUAL(st.alerts.size(), 2U); // k2_a and k3_a are not this signer's
    }
    // Epoch 2: the erased genesis key served Set'_{e-1} only (not missing);
    // the carried-over epoch-2 key sits in Set'_2 and Set'_3.
    {
        const auto st{status(a, {{0, Pk(g_a), false, {}}, {2, Pk(k2_a), true, {}}, {3, Pk(k3_a), true, {}}}, &e2,
                             S2 + 5)};
        BOOST_CHECK(st.fresh_keys == (std::vector<Entry>{{0, Pk(g_a), false, {0, 1}},
                                                          {2, Pk(k2_a), true, {2, 3}},
                                                          {3, Pk(k3_a), true, {}}}));
        BOOST_CHECK(st.missing_seats.empty());
        BOOST_CHECK(st.alerts.empty());
        // A genesis key the store never had is missing for Set'_1 and alerted.
        const auto lost{status(a, {{2, Pk(k2_a), true, {}}, {3, Pk(k3_a), true, {}}}, &e2, S2 + 5)};
        BOOST_CHECK(lost.missing_seats == (std::vector<Missing>{{1, Pk(g_a)}}));
        BOOST_REQUIRE_EQUAL(lost.alerts.size(), 1U);
        BOOST_CHECK(has(lost.alerts[0], HexStr(Pk(g_a))));
    }
    // Held keys that the chain recorded under another first claim: D (no
    // bindings of its own) holds A's epoch-2 key (a leaked secret: D's own
    // binding of it would be inert) and a key a V1 binding claimed first;
    // a never-claimed key and an erased one raise nothing.
    {
        const Validator d{4};
        const auto v1_key{BlsKey(250)};
        node::FreshKeyBindingIndex with_v1{index};
        with_v1.ConnectBlock(S1 + 4, {V1Claim(c, Pk(v1_key))});
        const auto st{node::ComputeFreshSignerKeyStatus(
            d.vk, {{2, Pk(k2_a), true, {}}, {std::nullopt, Pk(v1_key), true, {}}, {0, Pk(unbound), true, {}},
                   {2, Pk(k2_b), false, {}}},
            &with_v1, &e1, p, S1 + 5)};
        BOOST_REQUIRE_EQUAL(st.alerts.size(), 2U);
        BOOST_CHECK(has(st.alerts[0], "fresh key " + HexStr(Pk(k2_a)) + " of fresh epoch 2 held by this signer was "
                                      "claimed first by another binding at height 270"));
        BOOST_CHECK(has(st.alerts[0], "inert"));
        BOOST_CHECK(has(st.alerts[1], "fresh key " + HexStr(Pk(v1_key)) + " held by this signer was claimed first "
                                      "by another binding at height " + std::to_string(S1 + 4)));
        // The owner of a key is never told it was claimed elsewhere.
        for (const std::string& alert : status(a, {{2, Pk(k2_a), true, {}}}, &e1, S1 + 5).alerts) {
            BOOST_CHECK(!has(alert, "claimed first"));
        }
    }
    // Epoch 4: bindings and seats older than e - 2 are no longer reported;
    // B's void epoch-2 seat (target e - 2) still is.
    {
        BOOST_CHECK(status(c, {{0, Pk(g_c), true, {}}}, &e4, S4 + 5).alerts.empty());
        const auto st{status(b, {{0, Pk(g_b), true, {}}, {2, Pk(k2_b), true, {}}}, &e4, S4 + 5)};
        BOOST_REQUIRE_EQUAL(st.alerts.size(), 2U);
        BOOST_CHECK(has(st.alerts[0], HexStr(Pk(x2_b))));
        BOOST_CHECK(has(st.alerts[1], "seat for fresh epoch 2 is void"));
    }
}

BOOST_FIXTURE_TEST_CASE(fresh_key_signers_on_chain, FreshCommitteeChainFixture)
{
    // Four validators (A 15, B 1, C 10, D 8), MIN_FINALITY_SET = 2, and a
    // reorg horizon of 10 blocks: the erasure depth.
    constexpr int HORIZON{10};
    PrepareFinalityChain(/*min_finality_set=*/2, /*reorg_horizon=*/HORIZON, /*with_unbound_c=*/false,
                         /*with_bound_c=*/true, /*with_bound_d=*/true);
    const int M{m_M};
    const int R_KEY{M + 3};
    const int R_F{M + 11};
    const int G{R_F - 1};
    const int E{SCALED_E};
    const int start1{R_F + E};
    const int start2{R_F + 2 * E};
    const int start3{R_F + 3 * E};
    BOOST_REQUIRE(modern::IsCheckpointHeight(G, M, SCALED_INTERVAL));
    MutableConsensus().fresh_lineage = Consensus::FreshLineageParams{
        .activation_height = R_F, .max_epoch_extension = SCALED_MAX_EXTENSION, .staging_signer = true};
    MutableConsensus().fresh_lineage->key_height = R_KEY;
    const Consensus::Params& consensus{m_node.chainman->GetConsensus()};
    BOOST_REQUIRE(node::FreshKeyModeConfigured(consensus));
    BOOST_REQUIRE_EQUAL(node::FreshKeyErasureHorizon(consensus), HORIZON);
    constexpr auto npos{std::string::npos};
    using Entry = node::FreshSignerKeyEntry;
    using Missing = node::FreshMissingSeat;

    std::string error;
    const fs::path dir_a{m_path_root / "fresh-signer-a"};             // A, with its fresh key store
    const fs::path dir_a_plain{m_path_root / "fresh-signer-a-plain"}; // the same A without one
    const fs::path dir_c{m_path_root / "fresh-signer-c"};             // C, with its fresh key store
    const fs::path dir_c_old{m_path_root / "fresh-signer-c-backup"};  // C's identity restored elsewhere

    //! A new key from the fresh key store of (dir, vk) (a fresh instance:
    //! the signer rewrites the file when it erases).
    const auto generate{[&](const fs::path& dir, const ValidatorKeyBytes& vk, const uint64_t epoch) {
        node::FreshKeyStore store;
        std::string err;
        BOOST_REQUIRE_MESSAGE(store.Open(dir, m_domain, vk, err), err);
        const auto key{store.Generate(epoch, err)};
        BOOST_REQUIRE_MESSAGE(key, err);
        return *key;
    }};
    //! The store record of `sk` as it is on disk now.
    const auto stored{[&](const fs::path& dir, const ValidatorKeyBytes& vk, const bls::SecretKey& sk) {
        node::FreshKeyStore store;
        std::string err;
        BOOST_REQUIRE_MESSAGE(store.Open(dir, m_domain, vk, err), err);
        const auto record{store.RecordFor(Pk(sk))};
        BOOST_REQUIRE(record);
        BOOST_CHECK_EQUAL(store.SecretFor(Pk(sk)).has_value(), record->Held());
        return *record;
    }};
    const auto store_bytes{[&](const fs::path& dir, const ValidatorKeyBytes& vk) {
        return ReadFileBytes(node::FreshKeyStore::StatePath(dir, m_domain, vk));
    }};
    const auto verifies{[](const node::FreshLineageSig& sig, const bls::SecretKey& sk, const uint256& digest) {
        const auto decoded{bls::Signature::Decode(sig.signature)};
        return decoded && bls::Verify(sk.GetPublicKey(), std::span<const unsigned char>(digest.begin(), 32), *decoded);
    }};
    const auto has{[](const std::string& text, const std::string& part) { return text.find(part) != npos; }};

    // Persistent staging signers: A with a fresh key store, the same A
    // without one, and C with a fresh key store.
    node::FinalitySigner signer_a;
    node::FinalitySigner plain_a;
    node::FinalitySigner signer_c;
    BOOST_REQUIRE_MESSAGE(signer_a.SetKeysPersistent({m_bls_a}, m_vk_a, m_domain, dir_a, error), error);
    BOOST_REQUIRE_MESSAGE(plain_a.SetKeysPersistent({m_bls_a}, m_vk_a, m_domain, dir_a_plain, error), error);
    BOOST_REQUIRE_MESSAGE(signer_c.SetKeysPersistent({m_bls_c}, m_vk_c, m_domain, dir_c, error), error);
    // A persistent signer reads fresh keys only from its store.
    BOOST_CHECK(!signer_a.SetFreshKeys({BlsKey(150)}, error));
    BOOST_CHECK_EQUAL(error, "a persistent finality signer reads its fresh keys only from its fresh key store");
    node::FinalitySignaturePool pool_a;
    node::FinalitySignaturePool pool_plain;
    node::FinalitySignaturePool pool_c;

    //! One staking-loop pass (V1, then the fresh lineage).
    const auto pass{[&](node::FinalitySigner& signer, node::FinalitySignaturePool& pool) {
        LOCK(cs_main);
        Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
        return signer.MaybeSign(Finality(), chainstate.m_chain, consensus, pool);
    }};
    //! The fresh-lineage pass alone.
    const auto sign_fresh{[&](node::FinalitySigner& signer, node::FinalitySignaturePool& pool) {
        LOCK(cs_main);
        Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
        return signer.MaybeSignFresh(Finality(), chainstate.m_chain, consensus, pool.Fresh());
    }};
    //! A pass of every persistent signer: A's V1 votes, watermark, error and
    //! journal are identical with and without a fresh key store.
    const auto sign_all{[&]() {
        const auto with_store{pass(signer_a, pool_a)};
        const auto without_store{pass(plain_a, pool_plain)};
        pass(signer_c, pool_c);
        BOOST_CHECK(with_store == without_store);
        BOOST_CHECK_EQUAL(signer_a.LastSignedHeight(), plain_a.LastSignedHeight());
        BOOST_CHECK_EQUAL(signer_a.LastError(), plain_a.LastError());
        BOOST_CHECK_MESSAGE(signer_a.LastError().empty(), signer_a.LastError());
        BOOST_CHECK(ReadFileBytes(node::FinalitySignerStore::StatePath(dir_a, m_domain, m_vk_a)) ==
                    ReadFileBytes(node::FinalitySignerStore::StatePath(dir_a_plain, m_domain, m_vk_a)));
        return with_store;
    }};
    // The first pass at M - 1 establishes the empty V1 journals.
    BOOST_CHECK(sign_all().empty());

    // 1. A's and C's stores generate their genesis keys (random, durable,
    //    outside any wallet); B and D bring their own. All four bind at R_key.
    ProduceTo(R_KEY - 1, m_vk_a);
    sign_all();
    const bls::SecretKey g_a{generate(dir_a, m_vk_a, 0)};
    const bls::SecretKey g_c{generate(dir_c, m_vk_c, 0)};
    const bls::SecretKey g_b{BlsKey(151)}, g_d{BlsKey(152)};
    BOOST_CHECK(Pk(g_a) != Pk(m_bls_a));
    BOOST_CHECK(Pk(g_c) != Pk(m_bls_c));
    CMutableTransaction tx_genesis;
    {
        std::vector<CScript> cells;
        std::vector<CMpaRecord> records;
        const auto add{[&](const CKey& id, const ValidatorKeyBytes& vk, const bls::SecretKey& sk) {
            const auto b{MakeFreshBinding(id, vk, sk, 0)};
            cells.push_back(b.cell);
            records.push_back(b.record);
        }};
        add(m_validator_a, m_vk_a, g_a);
        add(m_validator_b, m_vk_b, g_b);
        add(m_validator_c, m_vk_c, g_c);
        add(m_validator_d, m_vk_d, g_d);
        tx_genesis = MakeTx(4, cells, records);
    }
    Produce(m_vk_a, {}, {tx_genesis});
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, R_KEY);
    sign_all();
    // Before the lineage: the keys, unseated, no alert, no error.
    BOOST_CHECK(signer_c.FreshKeyStatus().fresh_keys == (std::vector<Entry>{{0, Pk(g_c), true, {}}}));
    BOOST_CHECK(signer_c.FreshKeyStatus().missing_seats.empty());
    BOOST_CHECK(signer_c.FreshKeyStatus().alerts.empty());
    BOOST_CHECK(signer_c.FreshLastError().empty());
    BOOST_CHECK(plain_a.FreshKeyStatus().fresh_keys.empty());
    BOOST_CHECK(plain_a.FreshLastError().empty());

    // 2. At R_f - 1 the projection names the lineage: the fresh journals are
    //    established; Set'_0 = Set'_1 seat A on its genesis key; the signer
    //    without a store reports both seats as missing.
    ProduceTo(G, m_vk_a);
    sign_all();
    for (const node::FinalitySigner* signer : {&signer_a, &plain_a, &signer_c}) {
        BOOST_CHECK(signer->FreshStore().IsOpen() && signer->FreshStore().State());
    }
    BOOST_CHECK(signer_a.FreshKeyStatus().fresh_keys == (std::vector<Entry>{{0, Pk(g_a), true, {0, 1}}}));
    BOOST_CHECK(signer_a.FreshKeyStatus().missing_seats.empty());
    BOOST_CHECK_MESSAGE(signer_a.FreshLastError().empty(), signer_a.FreshLastError());
    BOOST_CHECK(plain_a.FreshKeyStatus().missing_seats == (std::vector<Missing>{{0, Pk(g_a)}, {1, Pk(g_a)}}));
    BOOST_CHECK_MESSAGE(has(plain_a.FreshLastError(),
                            "missing fresh-lineage key for current fresh epoch 0 committee BLS public key " +
                                HexStr(Pk(g_a))),
                        plain_a.FreshLastError());
    BOOST_REQUIRE(pool_plain.Fresh().LocalSigner().has_value());
    BOOST_CHECK(pool_plain.Fresh().LocalSigner()->missing_seats == plain_a.FreshKeyStatus().missing_seats);
    BOOST_CHECK_EQUAL(pool_plain.Fresh().LocalSigner()->last_error, plain_a.FreshLastError());
    BOOST_REQUIRE(pool_a.Fresh().LocalSigner().has_value());
    BOOST_CHECK(pool_a.Fresh().LocalSigner()->fresh_keys == signer_a.FreshKeyStatus().fresh_keys);

    // 3. G + 5 becomes signable at G + 8: the store-backed signers vote with
    //    their fresh keys; the signer without a store votes nothing fresh.
    ProduceTo(G + 8, m_vk_a);
    sign_all();
    const auto fresh0{Fresh()};
    BOOST_REQUIRE(fresh0 && fresh0->bootstrapped && fresh0->current);
    const auto set0{fresh0->current};
    BOOST_CHECK(SeatKey(set0, m_vk_a) == Opt(g_a));
    BOOST_CHECK(SeatKey(set0, m_vk_c) == Opt(g_c));
    BOOST_CHECK_EQUAL(signer_a.FreshLastSignedHeight(), G + 5);
    BOOST_CHECK_EQUAL(signer_c.FreshLastSignedHeight(), G + 5);
    BOOST_CHECK_EQUAL(plain_a.FreshLastSignedHeight(), -1);
    BOOST_CHECK_EQUAL(pool_a.Fresh().SignatureCount(0, G + 5), 1U); // verified against Set'_0
    BOOST_CHECK_EQUAL(pool_c.Fresh().SignatureCount(0, G + 5), 1U);
    BOOST_CHECK_EQUAL(pool_plain.Fresh().SignatureCount(0, G + 5), 0U);
    BOOST_CHECK(signer_a.FreshStore().State()->last_signed_height == G + 5);
    BOOST_CHECK_EQUAL(plain_a.FreshStore().State()->last_signed_height, -1);
    const uint256 digest0{modern::FinalityDigestV2(m_domain, FreshFbForNext(G + 5))};

    // Offline signers: A with its genesis key (SetFreshKeys), B with its V1
    // key only, and B offering its V1 key as a fresh key.
    node::FinalitySigner offline_a;
    node::FinalitySigner v1_only_b;
    node::FinalitySigner v1_as_fresh_b;
    BOOST_CHECK(!offline_a.SetFreshKeys({g_a}, error));
    BOOST_CHECK_EQUAL(error, "set the validator identity (SetKeys) before its fresh keys");
    BOOST_REQUIRE_MESSAGE(offline_a.SetKeys({m_bls_a}, m_vk_a, error), error);
    {
        std::vector<bls::SecretKey> too_many;
        for (unsigned i{0}; i <= node::FinalitySigner::MAX_FRESH_KEYS; ++i) too_many.push_back(BlsKey(160 + i));
        BOOST_CHECK(!offline_a.SetFreshKeys(too_many, error));
        BOOST_CHECK(has(error, "at most"));
    }
    BOOST_REQUIRE_MESSAGE(offline_a.SetFreshKeys({g_a, g_a}, error), error); // duplicates collapse
    BOOST_REQUIRE_MESSAGE(v1_only_b.SetKeys({m_bls_b}, m_vk_b, error), error);
    BOOST_REQUIRE_MESSAGE(v1_as_fresh_b.SetKeys({m_bls_b}, m_vk_b, error), error);
    BOOST_REQUIRE_MESSAGE(v1_as_fresh_b.SetFreshKeys({m_bls_b}, error), error);
    node::FinalitySignaturePool scratch;
    {
        const auto votes{sign_fresh(offline_a, scratch)};
        BOOST_REQUIRE_EQUAL(votes.size(), 1U);
        BOOST_CHECK(votes[0].lineage_id == fresh0->lineage_id);
        BOOST_CHECK_EQUAL(votes[0].epoch, 0U);
        BOOST_CHECK_EQUAL(votes[0].height, static_cast<uint64_t>(G + 5));
        BOOST_CHECK(set0->IndexOf(m_vk_a) == votes[0].index);
        BOOST_CHECK(verifies(votes[0], g_a, digest0));
        BOOST_CHECK(!verifies(votes[0], m_bls_a, digest0));
        BOOST_CHECK_EQUAL(scratch.Fresh().SignatureCount(0, G + 5), 1U);
        // The offline key's epoch comes from its chain binding.
        BOOST_CHECK(offline_a.FreshKeyStatus().fresh_keys == (std::vector<Entry>{{0, Pk(g_a), true, {0, 1}}}));
        BOOST_CHECK(offline_a.FreshKeyStatus().missing_seats.empty());
        BOOST_CHECK(offline_a.FreshLastError().empty());
    }
    for (node::FinalitySigner* signer : {&v1_only_b, &v1_as_fresh_b}) {
        BOOST_CHECK(sign_fresh(*signer, scratch).empty());
        BOOST_CHECK_EQUAL(signer->FreshLastSignedHeight(), -1);
        BOOST_CHECK(signer->FreshKeyStatus().missing_seats == (std::vector<Missing>{{0, Pk(g_b)}, {1, Pk(g_b)}}));
        BOOST_CHECK_MESSAGE(has(signer->FreshLastError(),
                                "missing fresh-lineage key for current fresh epoch 0 committee BLS public key " +
                                    HexStr(Pk(g_b))),
                            signer->FreshLastError());
    }
    BOOST_CHECK(v1_only_b.FreshKeyStatus().fresh_keys.empty());
    BOOST_CHECK(v1_as_fresh_b.FreshKeyStatus().fresh_keys ==
                (std::vector<Entry>{{std::nullopt, Pk(m_bls_b), true, {}}}));
    BOOST_CHECK_EQUAL(scratch.Fresh().SignatureCount(0, G + 5), 1U); // nothing from B
    // SetKeys names the validator afresh and forgets its fresh keys.
    {
        node::FinalitySigner reset_a;
        BOOST_REQUIRE_MESSAGE(reset_a.SetKeys({m_bls_a}, m_vk_a, error), error);
        BOOST_REQUIRE_MESSAGE(reset_a.SetFreshKeys({g_a}, error), error);
        BOOST_REQUIRE_MESSAGE(reset_a.SetKeys({m_bls_a}, m_vk_a, error), error);
        node::FinalitySignaturePool reset_pool;
        BOOST_CHECK(sign_fresh(reset_a, reset_pool).empty());
        BOOST_CHECK(reset_a.FreshKeyStatus().fresh_keys.empty());
    }
    // The key status is reported even when the fresh journal refuses: C's
    // store copied into a directory without C's journal.
    {
        const fs::path dir_c_late{m_path_root / "fresh-signer-c-late"};
        fs::create_directories(dir_c_late);
        BOOST_REQUIRE(fs::copy_file(node::FreshKeyStore::StatePath(dir_c, m_domain, m_vk_c),
                                    node::FreshKeyStore::StatePath(dir_c_late, m_domain, m_vk_c),
                                    fs::copy_options::none));
        node::FinalitySigner late_c;
        BOOST_REQUIRE_MESSAGE(late_c.SetKeysPersistent({m_bls_c}, m_vk_c, m_domain, dir_c_late, error), error);
        node::FinalitySignaturePool late_pool;
        BOOST_CHECK(sign_fresh(late_c, late_pool).empty());
        BOOST_CHECK_MESSAGE(has(late_c.FreshLastError(), "fresh-lineage signer journal is absent"),
                            late_c.FreshLastError());
        BOOST_CHECK(late_c.FreshKeyStatus().fresh_keys == (std::vector<Entry>{{0, Pk(g_c), true, {0, 1}}}));
        const auto& local{late_pool.Fresh().LocalSigner()};
        BOOST_REQUIRE(local.has_value());
        BOOST_CHECK(local->fresh_keys == late_c.FreshKeyStatus().fresh_keys);
        BOOST_CHECK_EQUAL(local->last_error, late_c.FreshLastError());
        BOOST_CHECK_EQUAL(local->last_signed_height, -1);
    }

    // 4. Keys naming 2, bound in [R_f, start_1 - 1]: A and C from their
    //    stores, D its own; then an old backup of C's identity, restored
    //    elsewhere, generates and binds another key naming 2.
    const bls::SecretKey s_a{generate(dir_a, m_vk_a, 2)};
    const bls::SecretKey s_c{generate(dir_c, m_vk_c, 2)};
    const bls::SecretKey s_d{BlsKey(153)};
    {
        const auto ba{MakeFreshBinding(m_validator_a, m_vk_a, s_a, 2)};
        const auto bc{MakeFreshBinding(m_validator_c, m_vk_c, s_c, 2)};
        const auto bd{MakeFreshBinding(m_validator_d, m_vk_d, s_d, 2)};
        Produce(m_vk_a, {},
                {SpendTx(CTransaction{tx_genesis}, {ba.cell, bc.cell, bd.cell}, {ba.record, bc.record, bd.record},
                         /*relay=*/false)});
    }
    sign_all();
    BOOST_CHECK(signer_c.FreshKeyStatus().alerts.empty());
    BOOST_CHECK(signer_c.FreshKeyStatus().fresh_keys ==
                (std::vector<Entry>{{0, Pk(g_c), true, {0, 1}}, {2, Pk(s_c), true, {}}}));
    const bls::SecretKey x_c{generate(dir_c_old, m_vk_c, 2)};
    {
        // The restored identity holds no key of C's own store.
        node::FreshKeyStore old_store;
        BOOST_REQUIRE_MESSAGE(old_store.Open(dir_c_old, m_domain, m_vk_c, error), error);
        BOOST_CHECK_EQUAL(old_store.Records().size(), 1U);
        BOOST_CHECK(!old_store.SecretFor(Pk(g_c)) && !old_store.SecretFor(Pk(s_c)));
        const auto bx{MakeFreshBinding(m_validator_c, m_vk_c, x_c, 2)};
        Produce(m_vk_a, {}, {MakeTx(5, {bx.cell}, {bx.record})});
    }
    {
        // C's signer raises both alerts, each logged once while it persists.
        int foreign_logged{0};
        int void_logged{0};
        DebugLogHelper foreign_log{"ALERT: fresh key " + HexStr(Pk(x_c)), [&](const std::string* line) {
                                       if (line) ++foreign_logged;
                                       return false;
                                   }};
        DebugLogHelper void_log{"ALERT: fresh-lineage seat for fresh epoch 2 is void", [&](const std::string* line) {
                                    if (line) ++void_logged;
                                    return false;
                                }};
        sign_all();
        sign_all();
        BOOST_CHECK_EQUAL(foreign_logged, 1);
        BOOST_CHECK_EQUAL(void_logged, 1);
    }
    {
        const auto& alerts{signer_c.FreshKeyStatus().alerts};
        BOOST_REQUIRE_EQUAL(alerts.size(), 2U);
        BOOST_CHECK(has(alerts[0], "fresh key " + HexStr(Pk(x_c)) + " bound for fresh epoch 2"));
        BOOST_CHECK(has(alerts[0], "is not in this signer's fresh key store"));
        BOOST_CHECK(has(alerts[1], "fresh-lineage seat for fresh epoch 2 is void: 2 fresh bindings"));
        BOOST_CHECK(has(alerts[1], HexStr(Pk(s_c))) && has(alerts[1], HexStr(Pk(x_c))));
        BOOST_REQUIRE(pool_c.Fresh().LocalSigner().has_value());
        BOOST_CHECK(pool_c.Fresh().LocalSigner()->alerts == alerts);
        // Alerts are not errors: C keeps signing epoch 0.
        BOOST_CHECK(signer_c.FreshLastError().empty());
        BOOST_CHECK(signer_a.FreshKeyStatus().alerts.empty());
    }

    // 5. The epoch-0 handovers (V1 and fresh, the fresh one on fresh keys).
    {
        const int cp0{G + 5};
        const auto fb{FreshFbForNext(cp0)};
        const auto set{SigningSetForNext(0)};
        ProduceFresh({V1CertForNext(M + 15)},
                     {FreshRecord(fb, *set, {{m_vk_a, &g_a}, {m_vk_b, &g_b}, {m_vk_c, &g_c}, {m_vk_d, &g_d}})});
    }
    BOOST_CHECK(Fresh()->handover_certified);

    // 6. Rotation into epoch 1: Set'_2 seats A and D on their new keys; C's
    //    seat is void, B sits out.
    ProduceTo(start1, m_vk_a);
    sign_all();
    std::shared_ptr<const node::FreshLineageSet> set2;
    {
        const auto fresh{Fresh()};
        BOOST_REQUIRE(fresh && fresh->next);
        BOOST_CHECK_EQUAL(fresh->epoch, 1U);
        set2 = fresh->next;
        BOOST_CHECK_EQUAL(set2->Size(), 2U);
        BOOST_CHECK(SeatKey(set2, m_vk_a) == Opt(s_a));
        BOOST_CHECK(SeatKey(set2, m_vk_d) == Opt(s_d));
        BOOST_CHECK(!SeatKey(set2, m_vk_c));
    }
    BOOST_CHECK(signer_a.FreshKeyStatus().fresh_keys ==
                (std::vector<Entry>{{0, Pk(g_a), true, {0, 1}}, {2, Pk(s_a), true, {2}}}));
    BOOST_CHECK(signer_c.FreshKeyStatus().fresh_keys ==
                (std::vector<Entry>{{0, Pk(g_c), true, {0, 1}}, {2, Pk(s_c), true, {}}}));
    Produce(m_vk_a, {V1CertForNext(M + 30)}); // V1's epoch-1 handover

    // 7. The fresh epoch-1 handover (checkpoint G + 35) on the genesis keys.
    {
        const int cp1{G + 35};
        ProduceTo(cp1 + SCALED_DEPTH - 1, m_vk_a);
        const auto fb{FreshFbForNext(cp1)};
        const auto set{SigningSetForNext(1)};
        ProduceFresh({}, {FreshRecord(fb, *set, {{m_vk_a, &g_a}, {m_vk_b, &g_b}, {m_vk_c, &g_c}, {m_vk_d, &g_d}})});
    }
    BOOST_CHECK(Fresh()->handover_certified);

    // 8. C's key naming 2 never got a seat (void): its committee is fixed
    //    since start_1, so it is erased at start_1 + horizon, not earlier;
    //    the genesis key, still current, stays.
    ProduceTo(start1 + HORIZON - 1, m_vk_a);
    sign_all();
    BOOST_CHECK(stored(dir_c, m_vk_c, s_c).Held());
    BOOST_CHECK(Contains(store_bytes(dir_c, m_vk_c), s_c.Bytes()));
    {
        ASSERT_DEBUG_LOG("erased fresh key " + HexStr(Pk(s_c)) + " of fresh epoch 2");
        ProduceTo(start1 + HORIZON, m_vk_a);
        sign_all();
    }
    BOOST_CHECK(!stored(dir_c, m_vk_c, s_c).Held());
    BOOST_CHECK(!Contains(store_bytes(dir_c, m_vk_c), s_c.Bytes()));
    BOOST_CHECK(stored(dir_c, m_vk_c, g_c).Held());
    BOOST_CHECK(Contains(store_bytes(dir_c, m_vk_c), g_c.Bytes()));
    BOOST_CHECK(signer_c.FreshKeyStatus().fresh_keys ==
                (std::vector<Entry>{{0, Pk(g_c), true, {0, 1}}, {2, Pk(s_c), false, {}}}));
    // An erased key is the store's own: only the backup's key stays foreign.
    BOOST_CHECK_EQUAL(signer_c.FreshKeyStatus().alerts.size(), 2U);
    BOOST_CHECK(signer_c.FreshKeyStatus().missing_seats.empty());
    BOOST_CHECK(signer_c.FreshLastError().empty());

    // 9. V1's epoch-2 handover, then the rotation into epoch 2: nobody named
    //    3, so Set'_3 carries Set'_2 over (A and D on their epoch-2 keys).
    ProduceTo(M + 63, m_vk_a);
    Produce(m_vk_a, {V1CertForNext(M + 60)});
    ProduceTo(start2, m_vk_a);
    sign_all();
    {
        const auto fresh{Fresh()};
        BOOST_REQUIRE(fresh && fresh->next);
        BOOST_CHECK_EQUAL(fresh->epoch, 2U);
        BOOST_CHECK(fresh->current->set_hash == set2->set_hash);
        BOOST_CHECK(fresh->next->members == fresh->current->members);
    }

    // 10. Offline keys rotate per epoch: G + 65 (epoch 2) is signable at
    //     G + 68; with the genesis key alone A cannot sign it, with its
    //     epoch-2 key it can (and the genesis key cannot have).
    const int cp2{G + 65};
    ProduceTo(cp2 + SCALED_DEPTH, m_vk_a);
    {
        node::FinalitySignaturePool scratch2;
        for (const auto& vote : sign_fresh(offline_a, scratch2)) BOOST_CHECK(vote.epoch != 2);
        BOOST_CHECK(offline_a.FreshKeyStatus().missing_seats == (std::vector<Missing>{{2, Pk(s_a)}, {3, Pk(s_a)}}));
        BOOST_CHECK_MESSAGE(has(offline_a.FreshLastError(), "missing fresh-lineage key for current fresh epoch 2"),
                            offline_a.FreshLastError());
        BOOST_REQUIRE_MESSAGE(offline_a.SetFreshKeys({g_a, s_a}, error), error);
        const auto votes{sign_fresh(offline_a, scratch2)};
        const auto vote{std::find_if(votes.begin(), votes.end(), [&](const node::FreshLineageSig& v) {
            return v.height == static_cast<uint64_t>(cp2);
        })};
        BOOST_REQUIRE(vote != votes.end());
        BOOST_CHECK_EQUAL(vote->epoch, 2U);
        const uint256 digest2{modern::FinalityDigestV2(m_domain, FreshFbForNext(cp2))};
        BOOST_CHECK(verifies(*vote, s_a, digest2));
        BOOST_CHECK(!verifies(*vote, g_a, digest2));
        BOOST_CHECK(!verifies(*vote, m_bls_a, digest2));
        BOOST_CHECK_EQUAL(scratch2.Fresh().SignatureCount(2, cp2), 1U);
        BOOST_CHECK(offline_a.FreshKeyStatus().fresh_keys ==
                    (std::vector<Entry>{{0, Pk(g_a), true, {0, 1}}, {2, Pk(s_a), true, {2, 3}}}));
        BOOST_CHECK(offline_a.FreshKeyStatus().missing_seats.empty());
        BOOST_CHECK(offline_a.FreshLastError().empty());
    }
    // The fresh epoch-2 handover by Set'_2 on the epoch-2 keys.
    {
        const auto fb{FreshFbForNext(cp2)};
        const auto set{SigningSetForNext(2)};
        BOOST_CHECK(set->set_hash == set2->set_hash);
        ProduceFresh({}, {FreshRecord(fb, *set, {{m_vk_a, &s_a}, {m_vk_d, &s_d}})});
    }
    BOOST_CHECK(Fresh()->handover_certified);

    // 11. The genesis keys (last seated in epoch 1) are held through
    //     start_2 + horizon - 1 and erased by the signers' pass at
    //     start_2 + horizon: record ERASED, no secret byte left in the file.
    ProduceTo(start2 + HORIZON - 1, m_vk_a);
    sign_all();
    BOOST_CHECK(stored(dir_a, m_vk_a, g_a).Held());
    BOOST_CHECK(stored(dir_c, m_vk_c, g_c).Held());
    BOOST_CHECK(Contains(store_bytes(dir_a, m_vk_a), g_a.Bytes()));
    {
        ASSERT_DEBUG_LOG("erased fresh key " + HexStr(Pk(g_a)) + " of fresh epoch 0");
        ASSERT_DEBUG_LOG("erased fresh key " + HexStr(Pk(g_c)) + " of fresh epoch 0");
        ProduceTo(start2 + HORIZON, m_vk_a);
        sign_all();
    }
    {
        const auto record{stored(dir_a, m_vk_a, g_a)};
        BOOST_CHECK(record.state == node::FreshKeyState::ERASED);
        BOOST_CHECK_EQUAL(record.target_epoch, 0U);
        const auto bytes{store_bytes(dir_a, m_vk_a)};
        BOOST_CHECK(!Contains(bytes, g_a.Bytes()));
        BOOST_CHECK(Contains(bytes, s_a.Bytes()));
        BOOST_CHECK(stored(dir_a, m_vk_a, s_a).Held());
        BOOST_CHECK(!stored(dir_c, m_vk_c, g_c).Held());
        BOOST_CHECK(!Contains(store_bytes(dir_c, m_vk_c), g_c.Bytes()));
    }
    BOOST_CHECK(signer_a.FreshKeyStatus().fresh_keys ==
                (std::vector<Entry>{{0, Pk(g_a), false, {0, 1}}, {2, Pk(s_a), true, {2, 3}}}));
    BOOST_CHECK(signer_a.FreshKeyStatus().missing_seats.empty());
    BOOST_CHECK_MESSAGE(signer_a.FreshLastError().empty(), signer_a.FreshLastError());
    BOOST_CHECK(signer_c.FreshKeyStatus().fresh_keys ==
                (std::vector<Entry>{{0, Pk(g_c), false, {0, 1}}, {2, Pk(s_c), false, {}}}));

    // 12. Rotation into epoch 3: Set'_3 (carried over) is current and Set'_4
    //     carries it again. A's epoch-2 key stays seated, so it is kept past
    //     start_3 + horizon; without the carry-over it would be erasable.
    ProduceTo(start3, m_vk_a);
    sign_all();
    ProduceTo(start3 + HORIZON + 2, m_vk_a);
    sign_all();
    {
        const auto fresh{Fresh()};
        BOOST_REQUIRE(fresh && fresh->current && fresh->next);
        BOOST_CHECK_EQUAL(fresh->epoch, 3U);
        BOOST_CHECK(fresh->current->members == set2->members);
        BOOST_CHECK(fresh->next->members == set2->members);
        const int tip{Tip()->nHeight};
        BOOST_CHECK(!node::FreshKeyErasable(*fresh, Pk(s_a), 2, tip, HORIZON));
        node::FreshLineageState uncarried{*fresh};
        uncarried.current = node::MakeFreshLineageSet(fresh->previous2->members, 3); // the genesis keys
        uncarried.next = node::MakeFreshLineageSet(fresh->previous2->members, 4);
        BOOST_CHECK(node::FreshKeyErasable(uncarried, Pk(s_a), 2, tip, HORIZON));
    }
    BOOST_CHECK(stored(dir_a, m_vk_a, s_a).Held());
    BOOST_CHECK(Contains(store_bytes(dir_a, m_vk_a), s_a.Bytes()));
    BOOST_CHECK(signer_a.FreshKeyStatus().fresh_keys ==
                (std::vector<Entry>{{0, Pk(g_a), false, {1}}, {2, Pk(s_a), true, {2, 3, 4}}}));
    BOOST_CHECK(signer_a.FreshKeyStatus().missing_seats.empty());
    // A keeps voting on the carried-over key (the last signable checkpoint).
    const int last_signable{start3 + HORIZON + 2 - SCALED_DEPTH};
    const int last_checkpoint{last_signable - (last_signable - G) % SCALED_INTERVAL};
    BOOST_CHECK_EQUAL(signer_a.FreshLastSignedHeight(), last_checkpoint);
    BOOST_CHECK_EQUAL(pool_a.Fresh().SignatureCount(3, last_checkpoint), 1U);

    // 13. A restarted signer reloads the store: same keys and states, no
    //     repeat vote.
    {
        node::FinalitySigner restarted;
        BOOST_REQUIRE_MESSAGE(restarted.SetKeysPersistent({m_bls_a}, m_vk_a, m_domain, dir_a, error), error);
        node::FinalitySignaturePool scratch3;
        BOOST_CHECK(sign_fresh(restarted, scratch3).empty());
        BOOST_CHECK(restarted.FreshKeyStatus().fresh_keys == signer_a.FreshKeyStatus().fresh_keys);
        BOOST_CHECK_EQUAL(restarted.FreshLastSignedHeight(), signer_a.FreshLastSignedHeight());
        BOOST_CHECK(restarted.FreshLastError().empty());
    }
}

namespace {

//! The blockchain RPC table with the result doc check forced on.
struct StatusRpc {
    CRPCTable table;
    node::NodeContext& node;
    const bool previous_doc_check;

    explicit StatusRpc(node::NodeContext& n)
        : node{n}, previous_doc_check{gArgs.GetBoolArg("-rpcdoccheck", DEFAULT_RPC_DOC_CHECK)}
    {
        RegisterBlockchainRPCCommands(table);
        gArgs.ForceSetArg("-rpcdoccheck", "1");
    }
    ~StatusRpc() { gArgs.ForceSetArg("-rpcdoccheck", previous_doc_check ? "1" : "0"); }

    UniValue Call(const std::string& method)
    {
        JSONRPCRequest request;
        request.context = &node;
        request.strMethod = method;
        request.params = RPCConvertValues(method, {});
        if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
        try {
            return table.execute(request);
        } catch (const UniValue& error) {
            throw std::runtime_error(error.find_value("message").get_str());
        }
    }
};

//! The binding of `pk` in a status (nullptr when absent).
const node::FreshKeyChainStatus::Binding* FindBinding(const node::FreshKeyChainStatus& status, const BlsPubkeyBytes& pk)
{
    for (const auto& binding : status.bindings) {
        if (binding.bls_pubkey == pk) return &binding;
    }
    return nullptr;
}

//! The JSON entry of `pk` in getfinalitylineage's fresh_keys.bindings.
UniValue FindBindingJson(const UniValue& bindings, const BlsPubkeyBytes& pk)
{
    for (const UniValue& entry : bindings.getValues()) {
        if (entry["bls_pubkey"].get_str() == HexStr(pk)) return entry;
    }
    return UniValue{};
}

} // namespace

BOOST_FIXTURE_TEST_CASE(fresh_key_status_rpc_and_chain_interface, FreshCommitteeChainFixture)
{
    // Four validators (A 15, B 1, C 10, D 8), MIN_FINALITY_SET = 2.
    PrepareFinalityChain(/*min_finality_set=*/2, /*reorg_horizon=*/200, /*with_unbound_c=*/false,
                         /*with_bound_c=*/true, /*with_bound_d=*/true);
    const int M{m_M};
    const int R_KEY{M + 3};
    const int R_F{M + 11};
    const Consensus::Params& consensus{m_node.chainman->GetConsensus()};
    using Window = node::FreshBindingWindow::Status;
    StatusRpc rpc{m_node};
    const auto status{[&](const std::optional<ValidatorKeyBytes>& vk) {
        LOCK(cs_main);
        return node::FreshKeyStatusAt(Finality(), m_node.chainman->ActiveChain(), consensus, vk);
    }};
    const fs::path signer_dir{gArgs.GetDataDirNet() / node::FINALITY_SIGNER_DIRNAME};

    // 0. Without the fresh lineage (every shipped network) and with the
    //    pre-B-37 staging lineage (no R_key): nothing reports fresh keys.
    {
        BOOST_CHECK(!status(std::nullopt).configured);
        BOOST_CHECK_EQUAL(rpc.Call("getfinalitylineage").write(), R"({"configured":false})");
        const interfaces::FreshFinalityKeyStatus chain_status{m_node.chain->freshFinalityKeyStatus(m_vk_a, {})};
        BOOST_CHECK(!chain_status.configured && !chain_status.available);
        BOOST_CHECK(chain_status.signer_directory == signer_dir);
    }
    MutableConsensus().fresh_lineage = Consensus::FreshLineageParams{
        .activation_height = R_F, .max_epoch_extension = SCALED_MAX_EXTENSION, .staging_signer = true};
    {
        const node::FreshKeyChainStatus s{status(std::nullopt)};
        BOOST_CHECK(!s.configured && !s.available && s.bindings.empty() && s.seats.empty());
        const UniValue lineage{rpc.Call("getfinalitylineage")};
        BOOST_CHECK(lineage["configured"].get_bool());
        BOOST_CHECK(lineage.find_value("fresh_keys").isNull());
        BOOST_CHECK(!m_node.chain->freshFinalityKeyStatus(m_vk_a, {}).configured);
    }
    MutableConsensus().fresh_lineage->key_height = R_KEY;
    BOOST_REQUIRE(node::FreshKeyModeConfigured(consensus));

    // 1. Tip M - 1: the next block is below R_key, no window is open.
    {
        const node::FreshKeyChainStatus s{status(std::nullopt)};
        BOOST_CHECK(s.configured && s.available);
        BOOST_CHECK(s.key_height == R_KEY);
        BOOST_CHECK_EQUAL(s.activation_height, R_F);
        BOOST_CHECK(s.chain_domain == m_domain);
        BOOST_CHECK_EQUAL(s.tip_height, M - 1);
        BOOST_CHECK(s.window_status == Window::NOT_ACTIVE);
        BOOST_CHECK(!s.next_binding_epoch && !s.window_start);
        BOOST_CHECK(!s.bootstrapped && s.bindings.empty() && s.seats.empty());
        const UniValue keys{rpc.Call("getfinalitylineage")["fresh_keys"]};
        BOOST_REQUIRE(keys.isObject());
        BOOST_CHECK_EQUAL(keys["key_height"].getInt<int>(), R_KEY);
        BOOST_CHECK(keys["state_available"].get_bool());
        BOOST_CHECK_EQUAL(keys["next_binding_window"].get_str(), "not-active");
        BOOST_CHECK(keys.find_value("next_binding_epoch").isNull());
        BOOST_CHECK(keys.find_value("window_start").isNull());
        BOOST_CHECK(keys["bindings"].empty() && keys["committees"].empty());
        const interfaces::FreshFinalityKeyStatus chain_status{m_node.chain->freshFinalityKeyStatus(m_vk_a, {})};
        BOOST_CHECK(chain_status.configured && chain_status.available);
        BOOST_CHECK(chain_status.chain_domain == m_domain);
        BOOST_CHECK(chain_status.key_height == R_KEY);
        BOOST_CHECK_EQUAL(chain_status.activation_height, R_F);
        BOOST_CHECK_EQUAL(chain_status.next_binding_window, "not-active");
        BOOST_CHECK(!chain_status.next_binding_epoch);
        BOOST_CHECK(chain_status.signer_directory == signer_dir);
    }

    // 2. Tip R_key - 1: the next block opens the genesis window (target 0).
    ProduceTo(R_KEY - 1, m_vk_a);
    {
        const node::FreshKeyChainStatus s{status(std::nullopt)};
        BOOST_CHECK(s.window_status == Window::OPEN);
        BOOST_CHECK(s.next_binding_epoch == 0U);
        BOOST_CHECK(s.window_start == R_KEY);
        const UniValue keys{rpc.Call("getfinalitylineage")["fresh_keys"]};
        BOOST_CHECK_EQUAL(keys["next_binding_window"].get_str(), "open");
        BOOST_CHECK_EQUAL(keys["next_binding_epoch"].getInt<int>(), 0);
        BOOST_CHECK_EQUAL(keys["window_start"].getInt<int>(), R_KEY);
        const interfaces::FreshFinalityKeyStatus chain_status{m_node.chain->freshFinalityKeyStatus(m_vk_a, {})};
        BOOST_CHECK_EQUAL(chain_status.next_binding_window, "open");
        BOOST_CHECK(chain_status.next_binding_epoch == 0U);
        BOOST_CHECK(chain_status.window_start == R_KEY);
    }

    // 3. Genesis bindings at R_key. A's key comes from its fresh key store
    //    (A's persistent signer reads it); B also names epoch 1 (never
    //    counts) and epoch 2 (before its window opens).
    const fs::path dir_a{m_path_root / "status-signer-a"};
    bls::SecretKey g_a{BlsKey(171)};
    {
        node::FreshKeyStore store;
        std::string error;
        BOOST_REQUIRE_MESSAGE(store.Open(dir_a, m_domain, m_vk_a, error), error);
        const auto key{store.Generate(0, error)};
        BOOST_REQUIRE_MESSAGE(key, error);
        g_a = *key;
    }
    const bls::SecretKey g_b{BlsKey(172)}, g_c{BlsKey(173)}, g_d{BlsKey(174)};
    const bls::SecretKey b_one{BlsKey(175)}, b_early{BlsKey(176)}, c_second{BlsKey(177)};
    const bls::SecretKey a_foreign{BlsKey(179)}; // A's identity used elsewhere (an old backup)
    {
        std::vector<CScript> cells;
        std::vector<CMpaRecord> records;
        const auto add{[&](const CKey& id, const ValidatorKeyBytes& vk, const bls::SecretKey& sk, const uint64_t e) {
            const auto b{MakeFreshBinding(id, vk, sk, e)};
            cells.push_back(b.cell);
            records.push_back(b.record);
        }};
        add(m_validator_a, m_vk_a, g_a, 0);
        add(m_validator_b, m_vk_b, g_b, 0);
        add(m_validator_c, m_vk_c, g_c, 0);
        add(m_validator_d, m_vk_d, g_d, 0);
        add(m_validator_b, m_vk_b, b_one, 1);
        add(m_validator_b, m_vk_b, b_early, 2);
        Produce(m_vk_a, {}, {MakeTx(4, cells, records)});
        BOOST_REQUIRE_EQUAL(Tip()->nHeight, R_KEY);
    }
    {
        const node::FreshKeyChainStatus s{status(std::nullopt)};
        BOOST_CHECK_EQUAL(s.bindings.size(), 6U);
        for (const bls::SecretKey* genesis : std::initializer_list<const bls::SecretKey*>{&g_a, &g_b, &g_c, &g_d}) {
            const auto* b{FindBinding(s, Pk(*genesis))};
            BOOST_REQUIRE(b);
            BOOST_CHECK_EQUAL(b->epoch, 0U);
            BOOST_CHECK_EQUAL(b->height, R_KEY);
            BOOST_CHECK(b->counted && !b->voided);
        }
        for (const bls::SecretKey* never : {&b_one, &b_early}) {
            const auto* b{FindBinding(s, Pk(*never))};
            BOOST_REQUIRE(b);
            BOOST_CHECK(!b->counted && !b->voided);
        }
        BOOST_CHECK(s.window_status == Window::OPEN && s.next_binding_epoch == 0U); // still R_key..R_f-1
        // Ordered by (validator, epoch), then height; filtered by validator.
        for (size_t i{1}; i < s.bindings.size(); ++i) {
            const auto& p{s.bindings[i - 1]};
            const auto& q{s.bindings[i]};
            BOOST_CHECK(std::tie(p.validator_key, p.epoch, p.height) <= std::tie(q.validator_key, q.epoch, q.height));
        }
        const node::FreshKeyChainStatus b_only{status(m_vk_b)};
        BOOST_CHECK_EQUAL(b_only.bindings.size(), 3U);
        for (const auto& binding : b_only.bindings) BOOST_CHECK(binding.validator_key == m_vk_b);
        const UniValue bindings{rpc.Call("getfinalitylineage")["fresh_keys"]["bindings"]};
        BOOST_CHECK_EQUAL(bindings.size(), 6U);
        const UniValue a_json{FindBindingJson(bindings, Pk(g_a))};
        BOOST_REQUIRE(a_json.isObject());
        BOOST_CHECK_EQUAL(a_json["validator_key"].get_str(), HexStr(m_vk_a));
        BOOST_CHECK_EQUAL(a_json["epoch"].getInt<int>(), 0);
        BOOST_CHECK_EQUAL(a_json["height"].getInt<int>(), R_KEY);
        BOOST_CHECK(a_json["counted"].get_bool() && !a_json["void"].get_bool());
        BOOST_CHECK(!FindBindingJson(bindings, Pk(b_one))["counted"].get_bool());
    }

    // 4. C binds a second genesis key: both of C's genesis bindings are void.
    {
        const auto second{MakeFreshBinding(m_validator_c, m_vk_c, c_second, 0)};
        Produce(m_vk_a, {}, {MakeTx(5, {second.cell}, {second.record})});
    }
    {
        const node::FreshKeyChainStatus c_only{status(m_vk_c)};
        BOOST_REQUIRE_EQUAL(c_only.bindings.size(), 2U);
        for (const auto& binding : c_only.bindings) BOOST_CHECK(binding.counted && binding.voided);
        const interfaces::FreshFinalityKeyStatus chain_c{m_node.chain->freshFinalityKeyStatus(m_vk_c, {})};
        BOOST_REQUIRE_EQUAL(chain_c.bindings.size(), 2U);
        for (const auto& binding : chain_c.bindings) BOOST_CHECK(binding.counted && binding.voided && binding.epoch == 0);
        const UniValue bindings{rpc.Call("getfinalitylineage")["fresh_keys"]["bindings"]};
        BOOST_CHECK(FindBindingJson(bindings, Pk(c_second))["void"].get_bool());
        BOOST_CHECK(FindBindingJson(bindings, Pk(g_c))["void"].get_bool());
        BOOST_CHECK(!FindBindingJson(bindings, Pk(g_a))["void"].get_bool());
    }

    // 5. At R_f: Set'_0 = Set'_1 = {A, B, D} on their genesis keys (C's seat
    //    is void); the next window counts epoch 2 from R_f.
    ProduceTo(R_F, m_vk_a);
    {
        const node::FreshKeyChainStatus s{status(std::nullopt)};
        BOOST_CHECK(s.bootstrapped && !s.broken);
        BOOST_CHECK_EQUAL(s.epoch, 0U);
        BOOST_CHECK(s.window_status == Window::OPEN);
        BOOST_CHECK(s.next_binding_epoch == 2U);
        BOOST_CHECK(s.window_start == R_F);
        BOOST_REQUIRE_EQUAL(s.seats.size(), 6U);
        const std::map<ValidatorKeyBytes, std::pair<BlsPubkeyBytes, uint64_t>> expected{
            {m_vk_a, {Pk(g_a), 15}}, {m_vk_b, {Pk(g_b), 1}}, {m_vk_d, {Pk(g_d), 8}}};
        for (size_t i{0}; i < s.seats.size(); ++i) {
            const auto& seat{s.seats[i]};
            BOOST_CHECK_EQUAL(seat.epoch, i < 3 ? 0U : 1U);
            const auto it{expected.find(seat.validator_key)};
            BOOST_REQUIRE(it != expected.end());
            BOOST_CHECK(seat.bls_pubkey == it->second.first);
            BOOST_CHECK_EQUAL(seat.weight, it->second.second);
        }
        // B's early epoch-2 binding precedes the epoch-2 window [R_f, ...].
        BOOST_CHECK(!FindBinding(s, Pk(b_early))->counted);

        const UniValue keys{rpc.Call("getfinalitylineage")["fresh_keys"]};
        BOOST_CHECK_EQUAL(keys["next_binding_epoch"].getInt<int>(), 2);
        BOOST_CHECK_EQUAL(keys["window_start"].getInt<int>(), R_F);
        const UniValue committees{keys["committees"]};
        BOOST_REQUIRE_EQUAL(committees.size(), 2U);
        for (size_t i{0}; i < 2; ++i) {
            BOOST_CHECK_EQUAL(committees[i]["epoch"].getInt<int>(), static_cast<int>(i));
            const UniValue& members{committees[i]["members"]};
            BOOST_REQUIRE_EQUAL(members.size(), 3U);
            for (const UniValue& member : members.getValues()) {
                BOOST_CHECK(member["validator_key"].get_str() != HexStr(m_vk_c));
                BOOST_CHECK(member["bls_pubkey"].get_str() != HexStr(Pk(m_bls_a)));
            }
        }
        const interfaces::FreshFinalityKeyStatus chain_a{m_node.chain->freshFinalityKeyStatus(m_vk_a, {})};
        BOOST_CHECK(chain_a.bootstrapped && !chain_a.broken && chain_a.epoch == 0);
        BOOST_CHECK(chain_a.next_binding_epoch == 2U && chain_a.window_start == R_F);
        BOOST_REQUIRE_EQUAL(chain_a.seats.size(), 2U);
        for (const auto& seat : chain_a.seats) {
            BOOST_CHECK(seat.bls_pubkey == Pk(g_a));
            BOOST_CHECK_EQUAL(seat.weight, 15U);
        }
    }

    // 6. A's persistent signer reports into the chainstate pool, which
    //    getfinalitylineage's local_signer shows: its genesis key, seated in
    //    Set'_0 and Set'_1, no missing seat, no alert.
    node::FinalitySigner signer_a;
    std::string error;
    BOOST_REQUIRE_MESSAGE(signer_a.SetKeysPersistent({m_bls_a}, m_vk_a, m_domain, dir_a, error), error);
    const auto pass_a{[&] {
        LOCK(cs_main);
        Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
        signer_a.MaybeSignFresh(Finality(), chainstate.m_chain, consensus, chainstate.FinalitySignatures().Fresh());
    }};
    pass_a();
    {
        const UniValue local{rpc.Call("getfinalitylineage")["local_signer"]};
        BOOST_REQUIRE(local.isObject());
        const UniValue& keys{local["fresh_keys"]};
        BOOST_REQUIRE_EQUAL(keys.size(), 1U);
        BOOST_CHECK_EQUAL(keys[0]["epoch"].getInt<int>(), 0);
        BOOST_CHECK_EQUAL(keys[0]["bls_pubkey"].get_str(), HexStr(Pk(g_a)));
        BOOST_CHECK(keys[0]["held"].get_bool());
        BOOST_CHECK_EQUAL(keys[0]["seated_epochs"].write(), "[0,1]");
        BOOST_CHECK(local["missing_seats"].empty());
        BOOST_CHECK(local["alerts"].empty());
        BOOST_CHECK_EQUAL(local["last_error"].get_str(), "");
    }

    // 7. In fresh epoch 0, A's store binds its epoch-2 key and A's identity,
    //    held elsewhere, binds another: both count and void A's epoch-2 seat;
    //    the signer reports the foreign binding and the void seat.
    bls::SecretKey s_a{BlsKey(178)};
    {
        node::FreshKeyStore store;
        BOOST_REQUIRE_MESSAGE(store.Open(dir_a, m_domain, m_vk_a, error), error);
        const auto key{store.Generate(2, error)};
        BOOST_REQUIRE_MESSAGE(key, error);
        s_a = *key;
        const auto own{MakeFreshBinding(m_validator_a, m_vk_a, s_a, 2)};
        const auto foreign{MakeFreshBinding(m_validator_a, m_vk_a, a_foreign, 2)};
        Produce(m_vk_a, {}, {MakeTx(8, {own.cell, foreign.cell}, {own.record, foreign.record})});
        BOOST_REQUIRE_EQUAL(Tip()->nHeight, R_F + 1);
    }
    pass_a();
    {
        const node::FreshKeyChainStatus a_only{status(m_vk_a)};
        BOOST_REQUIRE_EQUAL(a_only.bindings.size(), 3U);
        for (const bls::SecretKey* key : std::initializer_list<const bls::SecretKey*>{&s_a, &a_foreign}) {
            const auto* b{FindBinding(a_only, Pk(*key))};
            BOOST_REQUIRE(b);
            BOOST_CHECK_EQUAL(b->epoch, 2U);
            BOOST_CHECK(b->counted && b->voided);
        }
        BOOST_CHECK(!FindBinding(a_only, Pk(g_a))->voided);
        const interfaces::FreshFinalityKeyStatus chain_a{m_node.chain->freshFinalityKeyStatus(m_vk_a, {})};
        BOOST_REQUIRE_EQUAL(chain_a.bindings.size(), 3U);
        size_t voided{0};
        for (const auto& binding : chain_a.bindings) voided += binding.voided;
        BOOST_CHECK_EQUAL(voided, 2U);

        const UniValue local{rpc.Call("getfinalitylineage")["local_signer"]};
        const UniValue& keys{local["fresh_keys"]};
        BOOST_REQUIRE_EQUAL(keys.size(), 2U);
        BOOST_CHECK_EQUAL(keys[1]["epoch"].getInt<int>(), 2);
        BOOST_CHECK_EQUAL(keys[1]["bls_pubkey"].get_str(), HexStr(Pk(s_a)));
        BOOST_CHECK(keys[1]["held"].get_bool());
        BOOST_CHECK(keys[1]["seated_epochs"].empty());
        const UniValue& alerts{local["alerts"]};
        BOOST_REQUIRE_EQUAL(alerts.size(), 2U);
        BOOST_CHECK_MESSAGE(alerts[0].get_str().find("fresh key " + HexStr(Pk(a_foreign)) + " bound for fresh epoch 2") !=
                                std::string::npos,
                            alerts[0].get_str());
        BOOST_CHECK_MESSAGE(alerts[1].get_str().find("seat for fresh epoch 2 is void") != std::string::npos,
                            alerts[1].get_str());
        BOOST_CHECK(local["missing_seats"].empty());
    }
}

BOOST_AUTO_TEST_SUITE_END()
