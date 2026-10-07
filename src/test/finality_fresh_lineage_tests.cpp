// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Fresh finality lineage (bridge V2 stage S3): the fresh-lineage
// state machine inside node::FinalityTracker (genesis at R_f from
// Snapshot(R_f - 1), handover-gated rotation, carry-over, its own break
// deadline) and the MPA type-11 carrier rule judged in block validation
// next to the unchanged V1 certificate, behind Consensus::Params::
// fresh_lineage (only the regtest -b3freshlineageheight knob sets it).
//
// The chain cases share FinalityChainFixture's scaled constants (E = 30,
// interval 5, depth 3, V1 extension 30) with fresh_lineage = {R_f = M + 11,
// extension 30}, so GENESIS_HEIGHT G = M + 10 lies on the V1 grid. The fixture
// cost is dominated by its scrypt legacy prefix, so the chain checks are
// grouped into two cases.
//
// The fresh lineage's own signer-journal namespace
// (node::FinalitySignerNamespace::FRESH_LINEAGE_V2) is covered by cases that
// need no chain.
//
// The fresh vote pool (node::FreshLineageSignaturePool), the regtest staging
// signer (node::FinalitySigner::MaybeSignFresh, V1 keys) and the
// block-assembly hook (node::AppendFreshLineageCertificate) have two chain
// cases: the pool's rules and aggregation into a consensus-accepted record,
// then the persistent signers (V1 equivalence, journals, V1 orphan lock,
// V1's absent-journal rule, the finality pin of a V2 certificate) feeding
// node::BlockAssembler.
//
// The Verifier V2 export (node/finality_fresh_export.h) and the RPCs
// getfinalitylineage, getfinalitylineagecertificate and the hidden vote
// relay (getfinalitylineagevotes, submitfinalitylineagevote) have a pure case
// for the calldata layout and history paths, a no-knob case, and RPC checks
// appended to the pool case's chain (no extra chain fixture).

#include <chain.h>
#include <common/args.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <crypto/bls.h>
#include <modern/finality_schedule.h>
#include <modern/finality_types.h>
#include <modern/finality_v2.h>
#include <modern/mpa.h>
#include <modern/payload_root.h>
#include <node/finality_binding_index.h>
#include <node/finality_fresh_export.h>
#include <node/finality_fresh_lineage.h>
#include <node/finality_fresh_signature.h>
#include <node/finality_signature.h>
#include <node/finality_signer_store.h>
#include <node/finality_tracker.h>
#include <node/miner.h>
#include <node/stake_tracker.h>
#include <node/validator_set.h>
#include <rpc/client.h>
#include <rpc/register.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <streams.h>
#include <test/util/finality_fixture.h>
#include <test/util/setup_common.h>
#include <util/fs.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <univalue.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

using b3test::FinalityChainFixture;
using modern::CertificatePlacementV2;
using modern::FinalizedBlockV2;
using node::FinalitySignerNamespace;
using node::FinalitySignerStore;
using node::FinalityTracker;
using node::FreshLineageSig;
using node::FreshLineageSignaturePool;
using node::FreshLineageState;

namespace {

constexpr int FRESH_EXTENSION{30};

std::vector<unsigned char> FileBytes(const fs::path& path)
{
    std::ifstream in{path.std_path(), std::ios::binary};
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return bytes;
}

std::string FileMagic(const fs::path& path)
{
    const auto bytes{FileBytes(path)};
    return std::string(bytes.begin(), bytes.begin() + std::min<size_t>(bytes.size(), 8));
}

//! Fresh-lineage journals (any lineage) present in `dir`.
size_t FreshJournalCount(const fs::path& dir)
{
    if (!fs::exists(dir)) return 0;
    size_t count{0};
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (fs::PathToString(entry.path().filename()).starts_with("finality_signer_v2_lineage_")) ++count;
    }
    return count;
}

std::string AcceptName(const FreshLineageSignaturePool::Accept a) { return FreshLineageSignaturePool::AcceptName(a); }

uint256 SetHashOf(const std::shared_ptr<const node::FreshLineageSet>& s) { return s ? s->set_hash : uint256{}; }
uint256 SnapHashOf(const std::shared_ptr<const node::ValidatorSetSnapshot>& s) { return s ? s->SetHash() : uint256{}; }

//! Every V1 field of the tracker state (State::fresh excluded).
bool SameV1State(const FinalityTracker::State& a, const FinalityTracker::State& b)
{
    return a.bootstrapped == b.bootstrapped && a.epoch == b.epoch && a.epoch_starts == b.epoch_starts &&
           SnapHashOf(a.bootstrap) == SnapHashOf(b.bootstrap) && SnapHashOf(a.previous) == SnapHashOf(b.previous) &&
           SnapHashOf(a.current) == SnapHashOf(b.current) && SnapHashOf(a.next) == SnapHashOf(b.next) &&
           a.handover_certified == b.handover_certified && a.lineage_broken == b.lineage_broken &&
           a.finalized == b.finalized;
}

bool SameFreshState(const std::optional<FreshLineageState>& a, const std::optional<FreshLineageState>& b)
{
    if (a.has_value() != b.has_value()) return false;
    if (!a) return true;
    return a->bootstrapped == b->bootstrapped && a->genesis_height == b->genesis_height &&
           a->genesis_block_hash == b->genesis_block_hash && a->lineage_id == b->lineage_id &&
           a->epoch == b->epoch && a->epoch_starts == b->epoch_starts &&
           SetHashOf(a->genesis) == SetHashOf(b->genesis) && SetHashOf(a->previous2) == SetHashOf(b->previous2) &&
           SetHashOf(a->previous) == SetHashOf(b->previous) &&
           SetHashOf(a->current) == SetHashOf(b->current) && SetHashOf(a->next) == SetHashOf(b->next) &&
           a->handover_certified == b->handover_certified && a->lineage_broken == b->lineage_broken &&
           a->finalized == b->finalized && a->history == b->history;
}

struct FreshLineageFixture : public FinalityChainFixture {
    int m_G{0};  // GENESIS_HEIGHT = R_f - 1
    int m_Rf{0}; // R_f

    void PrepareFresh(const int min_finality_set = 1)
    {
        PrepareFinalityChain(min_finality_set);
        m_G = m_M + 10;
        m_Rf = m_G + 1;
        MutableConsensus().fresh_lineage = Consensus::FreshLineageParams{
            .activation_height = m_Rf, .max_epoch_extension = FRESH_EXTENSION, .staging_signer = true};
    }

    modern::FreshHashAtFn ChainHashFn()
    {
        return [this](const int h) -> std::optional<uint256> {
            LOCK(cs_main);
            const CBlockIndex* index{m_node.chainman->ActiveChain()[h]};
            if (!index) return std::nullopt;
            return index->GetBlockHash();
        };
    }

    std::optional<FreshLineageState> Fresh() { return FinalityState().fresh; }

    //! The FinalizedBlockV2 the next block's projected fresh state expects
    //! for `checkpoint` (epoch = the fresh epoch of the checkpoint unless
    //! given). `ignore_broken` builds the object a broken lineage would have
    //! certified, to show it is refused.
    FinalizedBlockV2 FreshFbForNext(const int checkpoint, const std::optional<uint64_t> epoch = std::nullopt,
                                    const bool ignore_broken = false)
    {
        const auto projected{ProjectedNext()};
        BOOST_REQUIRE(projected.fresh && projected.fresh->bootstrapped);
        FreshLineageState state{*projected.fresh};
        if (ignore_broken) state.lineage_broken = false;
        const auto e{epoch ? epoch : modern::EpochOfHeight(state.epoch_starts, checkpoint)};
        BOOST_REQUIRE(e.has_value());
        const auto fb{node::ExpectedFinalizedBlockV2(*e, checkpoint, state, ChainHashFn(),
                                                     m_node.chainman->GetConsensus(), nullptr)};
        BOOST_REQUIRE(fb.has_value());
        return *fb;
    }

    //! A type-11 record over `fb` signed by A and/or B as members of `signing`.
    CMpaRecord FreshRecord(const FinalizedBlockV2& fb, const node::FreshLineageSet& signing, const bool sign_a = true,
                           const bool sign_b = true, const std::optional<uint256>& digest_override = std::nullopt)
    {
        const uint256 digest{digest_override ? *digest_override : modern::FinalityDigestV2(m_domain, fb)};
        modern::FinalityCertificate cert;
        cert.signer_bitmap.assign(modern::SignerBitmapBytes(signing.Size()), 0);
        std::vector<bls::Signature> sigs;
        for (uint32_t i{0}; i < signing.Size(); ++i) {
            const auto& pk{signing.members->Members()[i].bls_pubkey};
            const bls::SecretKey* sk{nullptr};
            if (pk == m_bls_a.GetPublicKey().Compressed() && sign_a) sk = &m_bls_a;
            if (pk == m_bls_b.GetPublicKey().Compressed() && sign_b) sk = &m_bls_b;
            if (!sk) continue;
            cert.signer_bitmap[i / 8] |= static_cast<unsigned char>(1U << (i % 8));
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
    //! Signed by the committee the next block's projected state assigns to fb.epoch.
    CMpaRecord FreshRecordForNext(const FinalizedBlockV2& fb, const bool sign_a = true, const bool sign_b = true,
                                  const std::optional<uint256>& digest_override = std::nullopt)
    {
        const auto projected{ProjectedNext()};
        BOOST_REQUIRE(projected.fresh.has_value());
        auto set{projected.fresh->SetForEpoch(fb.epoch)};
        if (!set) set = projected.fresh->current;
        BOOST_REQUIRE(set);
        return FreshRecord(fb, *set, sign_a, sign_b, digest_override);
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

    //! FinalityChainFixture::BuildPosBlock plus fresh-lineage records: V1
    //! pairs add a cell and a record, V2 records add only the record (type 11
    //! has no metadata cell); the payload-root cell is recomputed, the merkle
    //! root refreshed and the block re-signed. finality_fixture.h is untouched.
    CBlock BuildFreshBlock(const std::vector<std::pair<CScript, CMpaRecord>>& v1,
                           const std::vector<CMpaRecord>& v2, std::vector<CMutableTransaction> txs = {},
                           const int extra = 0)
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
        CBlock block{BuildPos(prev, key, round, 0, extra, std::move(txs))};
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
    void ProduceFresh(const std::vector<std::pair<CScript, CMpaRecord>>& v1, const std::vector<CMpaRecord>& v2,
                      std::vector<CMutableTransaction> txs = {})
    {
        const int prev_height{Tip()->nHeight};
        const CBlock block{BuildFreshBlock(v1, v2, std::move(txs))};
        BOOST_REQUIRE_MESSAGE(Probe(block).empty(), "block at " << prev_height + 1 << " invalid: " << Probe(block));
        BOOST_REQUIRE(Submit(block));
        BOOST_REQUIRE_EQUAL(Tip()->nHeight, prev_height + 1);
    }
    //! Full validity of the would-be next block (empty = valid); nothing is stored.
    std::string ProbeFresh(const std::vector<std::pair<CScript, CMpaRecord>>& v1, const std::vector<CMpaRecord>& v2,
                           std::vector<CMutableTransaction> txs = {})
    {
        return Probe(BuildFreshBlock(v1, v2, std::move(txs)));
    }
    void CheckRejected(const std::vector<CMpaRecord>& v2, const std::string& reason,
                       const std::vector<std::pair<CScript, CMpaRecord>>& v1 = {})
    {
        BOOST_CHECK_EQUAL(ProbeFresh(v1, v2), "bad-finality-cert, " + reason);
    }

    //! The FinalizedBlockV2 the TIP's fresh state expects for `checkpoint`:
    //! the object the vote pool reconstructs (FreshFbForNext projects one
    //! block further, as the consensus judge of the next block does).
    FinalizedBlockV2 FreshFbAtTip(const int checkpoint, const std::optional<uint64_t> epoch = std::nullopt)
    {
        const auto state{FinalityState()};
        BOOST_REQUIRE(state.fresh && state.fresh->bootstrapped);
        const auto e{epoch ? epoch : modern::EpochOfHeight(state.fresh->epoch_starts, checkpoint)};
        BOOST_REQUIRE(e.has_value());
        const auto fb{node::ExpectedFinalizedBlockV2(*e, checkpoint, *state.fresh, ChainHashFn(),
                                                     m_node.chainman->GetConsensus(), nullptr)};
        BOOST_REQUIRE(fb.has_value());
        return *fb;
    }
    //! The vote of (sk, vk) over `fb` as a member of the tip's Set'_{fb.epoch}
    //! (Set'_current when that epoch has no committee here).
    FreshLineageSig FreshVote(const bls::SecretKey& sk, const modern::ValidatorKeyBytes& vk, const FinalizedBlockV2& fb,
                              const std::optional<uint256>& digest_override = std::nullopt)
    {
        const auto state{FinalityState()};
        BOOST_REQUIRE(state.fresh && state.fresh->bootstrapped);
        auto set{state.fresh->SetForEpoch(fb.epoch)};
        if (!set) set = state.fresh->current;
        BOOST_REQUIRE(set);
        const auto index{set->IndexOf(vk)};
        BOOST_REQUIRE(index.has_value());
        const uint256 digest{digest_override ? *digest_override : modern::FinalityDigestV2(m_domain, fb)};
        FreshLineageSig sig;
        sig.lineage_id = fb.lineage_id;
        sig.epoch = fb.epoch;
        sig.height = fb.height;
        sig.index = *index;
        sig.signature = sk.Sign(std::span<const unsigned char>(digest.begin(), 32)).Compressed();
        return sig;
    }
    FreshLineageSignaturePool::Accept SubmitFresh(FreshLineageSignaturePool& pool, const FreshLineageSig& sig,
                                                  const Consensus::Params* params = nullptr)
    {
        LOCK(cs_main);
        return pool.Submit(sig, Finality(), m_node.chainman->ActiveChain(),
                           params ? *params : m_node.chainman->GetConsensus());
    }

    //! Snapshot(height) from independent stake/binding trackers synced to the tip.
    node::ValidatorSetSnapshot IndependentSnapshotAtTip()
    {
        LOCK(cs_main);
        node::StakeTracker stakes;
        node::FinalityBindingTracker bindings;
        const CChain& chain{m_node.chainman->ActiveChain()};
        const Consensus::Params& consensus{m_node.chainman->GetConsensus()};
        BOOST_REQUIRE(stakes.Sync(chain, m_node.chainman->m_blockman, consensus, *chain.Tip()));
        BOOST_REQUIRE(bindings.Sync(chain, m_node.chainman->m_blockman, consensus, *chain.Tip()));
        auto snap{node::ValidatorSetSnapshot::BuildAt(0, stakes, chain.Height(), bindings.Index())};
        BOOST_REQUIRE(snap.has_value());
        return *snap;
    }
};

// ---------------------------------------------- export and RPC helpers

//! "0x" + raw (EVM-order) bytes: the lineage RPCs' form of Ethereum values.
std::string Evm0x(const uint256& h) { return "0x" + HexStr(std::span<const unsigned char>(h.begin(), 32)); }
std::string Evm0x(std::span<const unsigned char> bytes) { return "0x" + HexStr(bytes); }

//! A "0x"-prefixed RPC hash back to its raw bytes.
uint256 FromEvm0x(const std::string& hex)
{
    BOOST_REQUIRE(hex.starts_with("0x"));
    const auto bytes{ParseHex(hex.substr(2))};
    BOOST_REQUIRE_EQUAL(bytes.size(), 32U);
    uint256 out;
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return out;
}

modern::FinalityHistoryPathV2 PathFromJSON(const UniValue& array)
{
    BOOST_REQUIRE_EQUAL(array.size(), modern::FINALITY_HISTORY_DEPTH);
    modern::FinalityHistoryPathV2 path;
    for (size_t i{0}; i < path.size(); ++i) path[i] = FromEvm0x(array[i].get_str());
    return path;
}

//! 32 distinct bytes, so a reversed byte order cannot go unnoticed.
uint256 Asym(const unsigned char seed)
{
    uint256 out;
    for (size_t i{0}; i < 32; ++i) out.begin()[i] = static_cast<unsigned char>(seed + 7 * i);
    return out;
}

//! ABI word `w` of the call arguments (after the 4-byte selector).
std::span<const unsigned char> ArgWord(std::span<const unsigned char> calldata, const size_t w)
{
    BOOST_REQUIRE(4 + (w + 1) * 32 <= calldata.size());
    return calldata.subspan(4 + w * 32, 32);
}
uint64_t ArgU64(std::span<const unsigned char> calldata, const size_t w)
{
    const auto word{ArgWord(calldata, w)};
    BOOST_REQUIRE(std::all_of(word.begin(), word.begin() + 24, [](unsigned char b) { return b == 0; }));
    return ReadBE64(word.data() + 24);
}
bool WordIs(std::span<const unsigned char> word, const uint256& h)
{
    return std::equal(word.begin(), word.end(), h.begin());
}

std::string VoteHex(const FreshLineageSig& sig)
{
    DataStream ss;
    ss << sig;
    return HexStr(ss);
}

//! A member set built by the real snapshot machinery (the construction of
//! finality_v2_tests): member i has weight weights[i] in whole B3.
struct ExportMembers {
    std::vector<bls::SecretKey> keys;
    std::shared_ptr<const node::ValidatorSetSnapshot> snapshot;

    explicit ExportMembers(const std::vector<uint64_t>& weights)
    {
        node::FinalityBindingIndex bindings;
        std::map<node::ValidatorKey, CAmount> w;
        std::vector<node::FinalityBindingIndex::Transition> ts;
        for (size_t i{0}; i < weights.size(); ++i) {
            std::array<unsigned char, 32> ikm{};
            ikm[0] = static_cast<unsigned char>(i + 1);
            ikm[31] = 0x7E;
            keys.push_back(*bls::SecretKey::FromIKM(ikm));
            modern::ValidatorKeyBytes vk{};
            vk[0] = static_cast<unsigned char>(i + 1);
            vk[31] = 0x45;
            ts.push_back({vk, {keys.back().GetPublicKey().Compressed(), 0, 1}});
            w[vk] = static_cast<CAmount>(weights[i]) * modern::FINALITY_WEIGHT_UNIT;
        }
        bindings.ConnectBlock(1, ts);
        const auto snap{node::ValidatorSetSnapshot::Build(/*epoch=*/0, w, bindings)};
        BOOST_REQUIRE(snap.has_value());
        snapshot = std::make_shared<const node::ValidatorSetSnapshot>(*snap);
    }

    //! A certificate over `digest` signed by the given member indices.
    modern::FinalityCertificate Sign(const uint256& digest, const std::vector<uint32_t>& signers) const
    {
        modern::FinalityCertificate cert;
        cert.signer_bitmap.assign(modern::SignerBitmapBytes(snapshot->Size()), 0);
        std::vector<bls::Signature> sigs;
        for (const uint32_t i : signers) {
            const auto& pk{snapshot->Members()[i].bls_pubkey};
            const auto key{std::find_if(keys.begin(), keys.end(), [&pk](const bls::SecretKey& k) {
                return k.GetPublicKey().Compressed() == pk;
            })};
            BOOST_REQUIRE(key != keys.end());
            cert.signer_bitmap[i / 8] |= static_cast<unsigned char>(1U << (i % 8));
            sigs.push_back(key->Sign(std::span<const unsigned char>(digest.begin(), 32)));
        }
        cert.aggregate_sig = bls::AggregateSignatures(sigs)->Compressed();
        return cert;
    }
};

/**
 * Calls the blockchain RPC table as bitcoin-cli would (the client conversion
 * table included) with the result-documentation check forced on, so every
 * returned key and type must match the RPC's help. The previous setting is
 * restored afterwards.
 */
struct LineageRpc {
    CRPCTable table;
    node::NodeContext& node;
    const bool previous_doc_check;

    explicit LineageRpc(node::NodeContext& n)
        : node{n}, previous_doc_check{gArgs.GetBoolArg("-rpcdoccheck", DEFAULT_RPC_DOC_CHECK)}
    {
        RegisterBlockchainRPCCommands(table);
        gArgs.ForceSetArg("-rpcdoccheck", "1");
    }
    ~LineageRpc() { gArgs.ForceSetArg("-rpcdoccheck", previous_doc_check ? "1" : "0"); }

    UniValue Call(const std::string& method, const std::vector<std::string>& args = {})
    {
        JSONRPCRequest request;
        request.context = &node;
        request.strMethod = method;
        request.params = RPCConvertValues(method, args);
        if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
        try {
            return table.execute(request);
        } catch (const UniValue& error) {
            throw std::runtime_error(error.find_value("message").get_str());
        }
    }
    //! The error message of a call expected to fail ("" when it succeeded).
    std::string Error(const std::string& method, const std::vector<std::string>& args = {})
    {
        try {
            Call(method, args);
        } catch (const std::runtime_error& e) {
            return e.what();
        }
        return "";
    }
};

bool Contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

} // namespace

BOOST_AUTO_TEST_SUITE(finality_fresh_lineage_tests)

// ------------------------------------------------------------- pure rules

BOOST_FIXTURE_TEST_CASE(placement_v2_order_and_names, BasicTestingSetup)
{
    Consensus::ModernPosParams pos{};
    pos.finality_epoch_blocks = 30;
    pos.checkpoint_interval = 5;
    pos.checkpoint_depth = 3;
    pos.max_epoch_extension = 30;
    pos.min_finality_set = 1;
    modern::ValidatorSetView set;
    set.validator_count = 1;
    modern::FreshLineageEpochView view;
    view.lineage_id = uint256::ONE;
    view.genesis_height = 100;
    view.current_epoch = 0;
    view.epoch_starts = {101};
    view.current_set = &set;
    const uint256 block_hash{uint256{uint8_t{0x55}}};
    const uint256 history_root{uint256{uint8_t{0x77}}};
    const modern::FreshHashAtFn hash_at{[&](int) -> std::optional<uint256> { return block_hash; }};
    modern::FreshHistoryRootAtFn history_at{[&](uint64_t) -> std::optional<uint256> { return history_root; }};
    modern::FreshHashAtFn withdrawal_at{[](int) -> std::optional<uint256> { return uint256{}; }};
    modern::FreshBridgeFlagsAtFn flags_at{[](int) -> std::optional<uint8_t> { return uint8_t{0}; }};
    FinalizedBlockV2 fb;
    fb.lineage_id = uint256::ONE;
    fb.height = 105;
    fb.block_hash = block_hash;
    fb.checkpoint_index = 1;
    fb.checkpoint_history_root = history_root;
    const auto check{[&](const FinalizedBlockV2& f, const int including) {
        return modern::CheckCertificatePlacementV2(f, including, view, pos, hash_at, history_at, withdrawal_at,
                                                   flags_at);
    }};
    BOOST_CHECK(check(fb, 108) == CertificatePlacementV2::OK);
    BOOST_CHECK(check(fb, 107) == CertificatePlacementV2::INSUFFICIENT_DEPTH);
    BOOST_CHECK(check(fb, 104) == CertificatePlacementV2::INSUFFICIENT_DEPTH); // above the including block
    {
        auto f{fb};
        f.lineage_id = uint256{uint8_t{2}};
        BOOST_CHECK(check(f, 108) == CertificatePlacementV2::LINEAGE_MISMATCH);
        f = fb;
        f.height = 100; // GENESIS_HEIGHT itself: index 0 is never signed
        f.checkpoint_index = 0;
        BOOST_CHECK(check(f, 108) == CertificatePlacementV2::NOT_CHECKPOINT);
        f.height = 95; // below the genesis
        BOOST_CHECK(check(f, 108) == CertificatePlacementV2::NOT_CHECKPOINT);
        f.height = 106;
        BOOST_CHECK(check(f, 110) == CertificatePlacementV2::NOT_CHECKPOINT);
        f = fb;
        f.epoch = 1;
        BOOST_CHECK(check(f, 108) == CertificatePlacementV2::EPOCH_WINDOW);
        f = fb;
        f.block_hash = uint256::ONE;
        BOOST_CHECK(check(f, 108) == CertificatePlacementV2::WRONG_BLOCK_HASH);
        f = fb;
        f.checkpoint_index = 2;
        BOOST_CHECK(check(f, 108) == CertificatePlacementV2::CHECKPOINT_INDEX_MISMATCH);
        f = fb;
        f.checkpoint_history_root = uint256::ONE;
        BOOST_CHECK(check(f, 108) == CertificatePlacementV2::HISTORY_ROOT_MISMATCH);
        f = fb;
        f.withdrawal_root = uint256::ONE;
        BOOST_CHECK(check(f, 108) == CertificatePlacementV2::WITHDRAWAL_ROOT_MISMATCH);
        f = fb;
        f.bridge_flags = modern::FINALITY_BRIDGE_FLAG_DEPOSITS_CLOSED;
        BOOST_CHECK(check(f, 108) == CertificatePlacementV2::BRIDGE_FLAGS_MISMATCH);
    }
    // Epoch relation: with epoch 1 starting at 106, checkpoint 105 is epoch 0.
    {
        modern::ValidatorSetView prev_set;
        prev_set.validator_count = 1;
        auto v{view};
        view.current_epoch = 1;
        view.epoch_starts = {101, 106};
        view.previous_set = &prev_set;
        auto f{fb};
        f.epoch = 1;
        BOOST_CHECK(check(f, 108) == CertificatePlacementV2::EPOCH_RELATION);
        BOOST_CHECK(check(fb, 108) == CertificatePlacementV2::OK); // delayed epoch-0 certificate
        view.previous_set = nullptr;
        BOOST_CHECK(check(fb, 108) == CertificatePlacementV2::EPOCH_WINDOW);
        view = v;
    }
    // Unavailable derived values fail closed, in order.
    history_at = [](uint64_t) -> std::optional<uint256> { return std::nullopt; };
    BOOST_CHECK(check(fb, 108) == CertificatePlacementV2::HISTORY_ROOT_UNAVAILABLE);
    history_at = [&](uint64_t k) -> std::optional<uint256> { return k == 1 ? std::optional{history_root} : std::nullopt; };
    withdrawal_at = [](int) -> std::optional<uint256> { return std::nullopt; };
    BOOST_CHECK(check(fb, 108) == CertificatePlacementV2::WITHDRAWAL_ROOT_UNAVAILABLE);
    withdrawal_at = [](int) -> std::optional<uint256> { return uint256{}; };
    flags_at = [](int) -> std::optional<uint8_t> { return std::nullopt; };
    BOOST_CHECK(check(fb, 108) == CertificatePlacementV2::BRIDGE_FLAGS_UNAVAILABLE);
    flags_at = [](int) -> std::optional<uint8_t> { return uint8_t{0}; };
    // Regression follows the index check and precedes every derived value:
    // a regressed index is refused without asking for its history root.
    view.finalized_height = 105;
    BOOST_CHECK(check(fb, 108) == CertificatePlacementV2::FINALITY_REGRESSION);
    {
        size_t history_calls{0};
        history_at = [&](uint64_t) -> std::optional<uint256> {
            ++history_calls;
            return std::nullopt;
        };
        BOOST_CHECK(check(fb, 108) == CertificatePlacementV2::FINALITY_REGRESSION);
        BOOST_CHECK_EQUAL(history_calls, 0U);
        auto f{fb};
        f.checkpoint_index = 2;
        BOOST_CHECK(check(f, 108) == CertificatePlacementV2::CHECKPOINT_INDEX_MISMATCH);
        history_at = [&](uint64_t) -> std::optional<uint256> { return history_root; };
    }
    view.finalized_height = 104;
    BOOST_CHECK(check(fb, 108) == CertificatePlacementV2::OK);
    // A broken lineage refuses everything, before the lineage id.
    view.lineage_broken = true;
    BOOST_CHECK(check(fb, 108) == CertificatePlacementV2::LINEAGE_BROKEN);
    view.lineage_broken = false;
    view.current_set = nullptr;
    BOOST_CHECK(check(fb, 108) == CertificatePlacementV2::NO_FINALITY_SET);
    // Stable names.
    BOOST_CHECK_EQUAL(modern::CertificatePlacementV2Name(CertificatePlacementV2::FINALITY_REGRESSION),
                      "fresh-finality-regression");
    BOOST_CHECK_EQUAL(modern::CertificatePlacementV2Name(CertificatePlacementV2::LINEAGE_MISMATCH),
                      "fresh-lineage-mismatch");
    BOOST_CHECK_EQUAL(modern::CertificatePlacementV2Name(CertificatePlacementV2::OK), "fresh-ok");
}

BOOST_FIXTURE_TEST_CASE(unbootstrapped_lineage_fails_closed, BasicTestingSetup)
{
    Consensus::Params params{Params().GetConsensus()};
    Consensus::ModernPosParams pos{};
    pos.finality_epoch_blocks = 30;
    pos.checkpoint_interval = 5;
    pos.checkpoint_depth = 3;
    pos.max_epoch_extension = 30;
    pos.min_finality_set = 1;
    params.modern_pos = pos;
    params.legacy_final_hash = uint256::ONE;
    params.fresh_lineage = Consensus::FreshLineageParams{.activation_height = 100, .max_epoch_extension = 30,
                                                         .staging_signer = true};
    const node::FreshSnapshotAtFn no_snapshot{[](int) { return std::optional<node::ValidatorSetSnapshot>{}; }};

    std::optional<FreshLineageState> fresh;
    node::ProjectFreshLineage(fresh, 99, uint256::ONE, params, no_snapshot);
    BOOST_CHECK(!fresh.has_value()); // below R_f nothing exists
    node::ProjectFreshLineage(fresh, 100, uint256::ONE, params, no_snapshot);
    BOOST_REQUIRE(fresh.has_value());
    BOOST_CHECK(!fresh->bootstrapped); // no Snapshot(R_f - 1): fail closed
    BOOST_CHECK_EQUAL(fresh->genesis_height, 99);
    BOOST_CHECK(fresh->epoch_starts == std::vector<int>{100});
    BOOST_CHECK(!fresh->SetSize(0).has_value());
    BOOST_CHECK(!node::FreshHistoryRootAt(*fresh, 1, [](int) { return std::optional<uint256>{uint256::ONE}; }, 5));
    node::ProjectFreshLineage(fresh, 500, uint256::ONE, params, no_snapshot);
    BOOST_CHECK(!fresh->bootstrapped && !fresh->lineage_broken); // nothing to break
    BOOST_CHECK(fresh->View().current_set == nullptr);
    const auto view{node::MakeLineageV2View(params, fresh)};
    BOOST_CHECK(view.configured && !view.bootstrapped && view.genesis_height == 99);

    // A type-11 record while the lineage is not bootstrapped (or not
    // configured) is fresh-cert-inactive; a coinbase without one passes.
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vout.emplace_back(0, CScript() << OP_TRUE);
    CBlock block;
    block.vtx.push_back(MakeTransactionRef(coinbase));
    CBlockIndex index;
    index.nHeight = 501;
    const modern::FreshHashAtFn hash_at{[](int) { return std::optional<uint256>{uint256::ONE}; }};
    std::optional<modern::FinalityCertificateV2Record> out;
    std::string error;
    BOOST_CHECK(node::JudgeFreshCoinbaseCertificate(block, index, params, fresh, hash_at, nullptr, out, error));
    BOOST_CHECK(!out.has_value());
    CMpaRecord rec;
    rec.payload_type = modern::MPA_TYPE_FINALITY_CERTIFICATE_V2;
    rec.payload_version = modern::MPA_VERSION_V1;
    rec.payload.assign(modern::FINALITY_CERTIFICATE_V2_RECORD_MIN, 0);
    coinbase.mpa = {rec};
    block.vtx[0] = MakeTransactionRef(coinbase);
    BOOST_CHECK(!node::JudgeFreshCoinbaseCertificate(block, index, params, fresh, hash_at, nullptr, out, error));
    BOOST_CHECK_EQUAL(error, "fresh-cert-inactive");
    Consensus::Params no_knob{params};
    no_knob.fresh_lineage.reset();
    node::ProjectFreshLineage(fresh, 501, uint256::ONE, no_knob, no_snapshot);
    BOOST_CHECK(!fresh.has_value()); // the knob absent: no fresh state at all
    error.clear();
    BOOST_CHECK(!node::JudgeFreshCoinbaseCertificate(block, index, no_knob, fresh, hash_at, nullptr, out, error));
    BOOST_CHECK_EQUAL(error, "fresh-cert-inactive");
    BOOST_CHECK(!node::MakeLineageV2View(no_knob, fresh).configured);

    // The bridge hooks without a bridge V2 envelope (this chain has none, as
    // mainnet and testnet): zero withdrawal root and flags 0 even without an
    // index. From V they follow node::FinalityWithdrawalRootV2 and
    // node::FinalityBridgeFlagsV2 (bridge_state_tests).
    BOOST_REQUIRE(!params.bridge_v2);
    BOOST_CHECK(*node::FreshLineageWithdrawalRoot(1'000, params, nullptr) == uint256{});
    BOOST_CHECK(node::FreshLineageBridgeFlags(1'000, params, nullptr) == std::optional<uint8_t>{0});
}

// ------------------------------------------------------------ chain cases

BOOST_FIXTURE_TEST_CASE(bootstrap_carrier_rules_rebuild_and_reorg, FreshLineageFixture)
{
    PrepareFresh();
    const int M{m_M};
    const int G{m_G};
    const int Rf{m_Rf};
    BOOST_REQUIRE(modern::IsCheckpointHeight(G, M, SCALED_INTERVAL));
    const Consensus::Params& consensus{m_node.chainman->GetConsensus()};

    // A shadow tracker over the same chain WITHOUT the knob: with no V2
    // records every V1 field must match the knob-enabled tracker at every height.
    Consensus::Params v1_only{consensus};
    v1_only.fresh_lineage.reset();
    FinalityTracker shadow;
    const auto check_v1_equivalence{[&]() {
        LOCK(cs_main);
        const CChain& chain{m_node.chainman->ActiveChain()};
        BOOST_REQUIRE(shadow.Sync(chain, m_node.chainman->m_blockman, v1_only, *chain.Tip()));
        const FinalityTracker::State& live{Finality().Current()};
        BOOST_CHECK_MESSAGE(SameV1State(live, shadow.Current()), "V1 state differs at height " << chain.Height());
        BOOST_CHECK(!shadow.Current().fresh.has_value());
        if (chain.Height() < Rf) BOOST_CHECK(!live.fresh.has_value());
    }};
    check_v1_equivalence();
    while (Tip()->nHeight < M + 7) {
        Produce(m_vk_a);
        check_v1_equivalence();
    }
    // A V1 certificate before the fresh lineage exists (V1 behaves as without the knob).
    Produce(m_vk_a, {V1CertForNext(M + 5)});
    check_v1_equivalence();
    BOOST_CHECK_EQUAL(FinalityState().finalized->height, M + 5);
    while (Tip()->nHeight < G - 1) {
        Produce(m_vk_a);
        check_v1_equivalence();
    }

    // Below R_f the MPA registry refuses type 11 (block G = R_f - 1).
    {
        CMpaRecord rec;
        rec.payload_type = modern::MPA_TYPE_FINALITY_CERTIFICATE_V2;
        rec.payload_version = modern::MPA_VERSION_V1;
        rec.payload.assign(modern::FINALITY_CERTIFICATE_V2_RECORD_MIN, 0);
        const std::string reason{ProbeFresh({}, {rec})};
        BOOST_CHECK_MESSAGE(reason.starts_with("bad-mpa, mpa-inactive-type"), reason);
    }
    Produce(m_vk_a); // G
    check_v1_equivalence();
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, G);
    BOOST_CHECK(!Fresh().has_value());

    // Genesis at R_f from Snapshot(G), derived here independently.
    const node::ValidatorSetSnapshot snap_g{IndependentSnapshotAtTip()};
    const uint256 genesis_hash{ChainHashAt(G)};
    const auto set0_header{modern::RulesetV2Header(snap_g.Header(), 0)};
    const auto set1_header{modern::RulesetV2Header(snap_g.Header(), 1)};
    const uint256 set0_hash{modern::ValidatorSetHash(set0_header)};
    const uint256 lineage_id{modern::FinalityLineageIdV2(m_domain, static_cast<uint64_t>(G), genesis_hash, set0_hash)};
    BOOST_CHECK(ProjectedNext().fresh.has_value() && ProjectedNext().fresh->bootstrapped);
    Produce(m_vk_a); // R_f
    check_v1_equivalence();
    {
        const auto fresh{Fresh()};
        BOOST_REQUIRE(fresh.has_value());
        BOOST_REQUIRE(fresh->bootstrapped);
        BOOST_CHECK_EQUAL(fresh->genesis_height, G);
        BOOST_CHECK_EQUAL(fresh->genesis_block_hash.GetHex(), genesis_hash.GetHex());
        BOOST_CHECK_EQUAL(fresh->lineage_id.GetHex(), lineage_id.GetHex());
        BOOST_CHECK_EQUAL(fresh->epoch, 0U);
        BOOST_CHECK(fresh->epoch_starts == std::vector<int>{Rf});
        BOOST_REQUIRE(fresh->genesis && fresh->current && fresh->next && !fresh->previous && !fresh->previous2);
        // Epoch 0 retains Set'_0 and Set'_1 only.
        BOOST_CHECK(fresh->SetHashForEpoch(0) == std::optional<uint256>{set0_hash});
        BOOST_CHECK(fresh->SetHashForEpoch(1) == std::optional<uint256>{fresh->next->set_hash});
        BOOST_CHECK(!fresh->SetHashForEpoch(2));
        BOOST_CHECK(!fresh->SetHashForEpoch(std::numeric_limits<uint64_t>::max()));
        BOOST_CHECK(fresh->current->header == set0_header);
        BOOST_CHECK_EQUAL(fresh->current->header.ruleset_version, modern::FINALITY_RULESET_V2);
        BOOST_CHECK_EQUAL(fresh->current->set_hash.GetHex(), set0_hash.GetHex());
        BOOST_CHECK_EQUAL(fresh->genesis->set_hash.GetHex(), set0_hash.GetHex());
        BOOST_CHECK(fresh->next->header == set1_header);
        BOOST_CHECK(fresh->current->set_hash != snap_g.SetHash()); // never the ruleset-1 hash
        BOOST_CHECK_EQUAL(fresh->current->Size(), 2U);
        BOOST_CHECK(!fresh->handover_certified && !fresh->lineage_broken && !fresh->finalized);
        BOOST_CHECK_EQUAL(fresh->history.count, 1U);
        const std::vector<uint256> leaf0{modern::FinalityHistoryLeaf(static_cast<uint64_t>(G), genesis_hash)};
        BOOST_CHECK_EQUAL(fresh->history.root.GetHex(), modern::FinalityHistoryRoot(leaf0)->GetHex());
        const auto view{node::MakeLineageV2View(consensus, fresh)};
        BOOST_CHECK(view.configured && view.bootstrapped && !view.broken);
        BOOST_CHECK(view.lineage_id == lineage_id && view.genesis_height == G && view.epoch == 0);
        BOOST_CHECK(view.current_set_hash == set0_hash && view.current_count == 2 && view.current_weight == 16);
        BOOST_CHECK(view.next_set_hash == modern::ValidatorSetHash(set1_header) && !view.finalized_height);
    }
    while (Tip()->nHeight < G + 4) {
        Produce(m_vk_a);
        check_v1_equivalence();
    }
    // Shallow: a certificate for G+5 at G+7 (depth 2 < 3).
    Produce(m_vk_a);
    Produce(m_vk_a);
    check_v1_equivalence();
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, G + 6);
    CheckRejected({FreshRecordForNext(FreshFbForNext(G + 5))}, "fresh-insufficient-depth");
    Produce(m_vk_a); // G+7 = M+17
    check_v1_equivalence();

    // Every rejection at G+8 (each a distinct, unstored block).
    const FinalizedBlockV2 good{FreshFbForNext(G + 5)};
    BOOST_CHECK_EQUAL(good.checkpoint_index, 1U);
    BOOST_CHECK(good.lineage_id == lineage_id);
    BOOST_CHECK(good.withdrawal_root.IsNull() && good.bridge_flags == 0);
    {
        const std::vector<uint256> leaf0{modern::FinalityHistoryLeaf(static_cast<uint64_t>(G), genesis_hash)};
        BOOST_CHECK_EQUAL(good.checkpoint_history_root.GetHex(), modern::FinalityHistoryRoot(leaf0)->GetHex());
    }
    {
        auto fb{good};
        fb.height = G + 6;
        fb.block_hash = ChainHashAt(G + 6);
        CheckRejected({FreshRecordForNext(fb)}, "fresh-not-checkpoint");
        fb = good;
        fb.checkpoint_index = 2;
        CheckRejected({FreshRecordForNext(fb)}, "fresh-checkpoint-index-mismatch");
        fb = good;
        fb.checkpoint_history_root = uint256::ONE;
        CheckRejected({FreshRecordForNext(fb)}, "fresh-history-root-mismatch");
        fb = good;
        fb.lineage_id = uint256::ONE;
        CheckRejected({FreshRecordForNext(fb)}, "fresh-lineage-mismatch");
        fb = good;
        auto ruleset1{set1_header};
        ruleset1.ruleset_version = modern::FINALITY_RULESET_V1;
        fb.validator_set_hash = modern::ValidatorSetHash(ruleset1);
        CheckRejected({FreshRecordForNext(fb)}, "fresh-wrong-successor-set");
        fb = good;
        fb.withdrawal_root = uint256::ONE;
        CheckRejected({FreshRecordForNext(fb)}, "fresh-withdrawal-root-mismatch");
        fb = good;
        fb.bridge_flags = modern::FINALITY_BRIDGE_FLAG_DEPOSITS_CLOSED;
        CheckRejected({FreshRecordForNext(fb)}, "fresh-bridge-flags-mismatch");
        fb = good;
        fb.epoch = 1; // no committee for epoch 1 is in force yet
        CheckRejected({FreshRecordForNext(fb)}, "fresh-cert-unknown-epoch-set");
    }
    {
        // A V1-tagged signature over the analogous V1 object.
        modern::FinalizedBlock v1_fb;
        v1_fb.height = good.height;
        v1_fb.block_hash = good.block_hash;
        v1_fb.withdrawal_root = good.withdrawal_root;
        v1_fb.validator_set_hash = good.validator_set_hash;
        v1_fb.epoch = good.epoch;
        CheckRejected({FreshRecordForNext(good, true, true, modern::FinalityDigest(m_domain, v1_fb))},
                      "fresh-bad-signature");
        // Weight alone (A: 15 of 16) without the headcount quorum (2 of 2).
        CheckRejected({FreshRecordForNext(good, true, false)}, "fresh-insufficient-headcount");
        // Two type-11 records in one coinbase.
        CheckRejected({FreshRecordForNext(good), FreshRecordForNext(good, true, false)},
                      "fresh-cert-duplicate-record");
        // A type-11 record outside the coinbase.
        const std::string reason{ProbeFresh({}, {}, {MakeTx(8, {}, {FreshRecordForNext(good)})})};
        BOOST_CHECK_EQUAL(reason, "bad-finality-cert-form, fresh-cert-record-not-in-coinbase");
    }
    // Candidate judgement (block assembly path) agrees with the block rule.
    {
        LOCK(cs_main);
        const auto decoded{modern::DecodeCertificatePayloadV2(FreshRecordForNext(good).payload, 2)};
        BOOST_REQUIRE(decoded.has_value());
        std::string error;
        BOOST_CHECK(Finality().JudgeCandidateCertificateV2(good, decoded->second, *Tip(), consensus, error));
        auto bad{good};
        bad.checkpoint_index = 3;
        BOOST_CHECK(!Finality().JudgeCandidateCertificateV2(bad, decoded->second, *Tip(), consensus, error));
        BOOST_CHECK_EQUAL(error, "fresh-checkpoint-index-mismatch");
    }
    const auto state_before_carrier{FinalityState()};

    // The V2 certificate for G+5 (index 1) and a V1 certificate for M+15 in
    // one coinbase, 3 deep: both accepted.
    BOOST_REQUIRE_EQUAL(G + 5, M + 15);
    const CMpaRecord good_record{FreshRecordForNext(good)};
    ProduceFresh({V1CertForNext(M + 15)}, {good_record}); // G+8
    const uint256 carrier_hash{Tip()->GetBlockHash()};
    {
        const auto s{FinalityState()};
        BOOST_REQUIRE(s.finalized.has_value());
        BOOST_CHECK_EQUAL(s.finalized->height, M + 15);
        BOOST_CHECK(s.handover_certified);
        BOOST_REQUIRE(s.fresh && s.fresh->finalized);
        const auto& f{*s.fresh->finalized};
        BOOST_CHECK_EQUAL(f.height, G + 5);
        BOOST_CHECK_EQUAL(f.epoch, 0U);
        BOOST_CHECK_EQUAL(f.checkpoint_index, 1U);
        BOOST_CHECK_EQUAL(f.certified_at, G + 8);
        BOOST_CHECK_EQUAL(f.block_hash.GetHex(), ChainHashAt(G + 5).GetHex());
        BOOST_CHECK_EQUAL(f.cert_id.GetHex(), modern::FinalityCertIdV2(good).GetHex());
        BOOST_CHECK_EQUAL(f.history_root.GetHex(), good.checkpoint_history_root.GetHex());
        BOOST_CHECK_EQUAL(s.fresh->history.count, 2U);
        const std::vector<uint256> leaves{modern::FinalityHistoryLeaf(static_cast<uint64_t>(G), genesis_hash),
                                          modern::FinalityHistoryLeaf(static_cast<uint64_t>(G + 5), ChainHashAt(G + 5))};
        BOOST_CHECK_EQUAL(s.fresh->history.root.GetHex(), modern::FinalityHistoryRoot(leaves)->GetHex());
        BOOST_CHECK(s.fresh->handover_certified);
        BOOST_CHECK_EQUAL(s.fresh->epoch, 0U);
        // The V1 lineage only saw its own certificate.
        BOOST_CHECK_EQUAL(s.epoch, 0U);
        BOOST_CHECK(s.finalized->block_hash == ChainHashAt(M + 15));
    }
    // Regression: the same checkpoint again (G+9).
    CheckRejected({FreshRecordForNext(good)}, "fresh-finality-regression");
    // The next checkpoint's root covers leaves 0..1 from the accumulator.
    Produce(m_vk_a); // G+9
    Produce(m_vk_a); // G+10
    {
        const FinalizedBlockV2 next{FreshFbForNext(G + 10)};
        BOOST_CHECK_EQUAL(next.checkpoint_index, 2U);
        const std::vector<uint256> leaves{modern::FinalityHistoryLeaf(static_cast<uint64_t>(G), genesis_hash),
                                          modern::FinalityHistoryLeaf(static_cast<uint64_t>(G + 5), ChainHashAt(G + 5))};
        BOOST_CHECK_EQUAL(next.checkpoint_history_root.GetHex(), modern::FinalityHistoryRoot(leaves)->GetHex());
    }
    const auto state_after{FinalityState()};

    // Rebuild: discarding the derived state reproduces it exactly.
    WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().ModernFinality().MarkDirty());
    {
        const auto rebuilt{FinalityState()};
        BOOST_CHECK(SameV1State(state_after, rebuilt));
        BOOST_CHECK(SameFreshState(state_after.fresh, rebuilt.fresh));
    }

    // Reorg across the V2 carrier: disconnecting it restores the state below
    // it; reconnecting the identical blocks restores the state above it.
    {
        BlockValidationState state;
        CBlockIndex* index{WITH_LOCK(cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(carrier_hash))};
        BOOST_REQUIRE(index != nullptr);
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().InvalidateBlock(state, index));
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().ActivateBestChain(state));
    }
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, G + 7);
    {
        const auto below{FinalityState()};
        BOOST_CHECK(SameV1State(state_before_carrier, below));
        BOOST_CHECK(SameFreshState(state_before_carrier.fresh, below.fresh));
        BOOST_REQUIRE(below.fresh.has_value());
        BOOST_CHECK(!below.fresh->finalized.has_value());
        BOOST_CHECK_EQUAL(below.fresh->history.count, 1U);
    }
    {
        CBlockIndex* index{WITH_LOCK(cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(carrier_hash))};
        BOOST_REQUIRE(index != nullptr);
        {
            LOCK(cs_main);
            m_node.chainman->ActiveChainstate().ResetBlockFailureFlags(index);
            m_node.chainman->RecalculateBestHeader();
        }
        BlockValidationState state;
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().ActivateBestChain(state));
    }
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, G + 10);
    {
        const auto above{FinalityState()};
        BOOST_CHECK(SameV1State(state_after, above));
        BOOST_CHECK(SameFreshState(state_after.fresh, above.fresh));
    }
}

BOOST_FIXTURE_TEST_CASE(rotation_carry_over_break_and_v1_independence, FreshLineageFixture)
{
    // MIN_FINALITY_SET = 2: B's revocation leaves a one-member snapshot,
    // so the next committee is carried over.
    PrepareFresh(/*min_finality_set=*/2);
    const int M{m_M};
    const int G{m_G};
    const int Rf{m_Rf};
    const int E{SCALED_E};

    ProduceTo(M + 17, m_vk_a);
    // Fresh epoch-0 handover (G+5) and V1 epoch-0 handover (M+15) at M+18.
    ProduceFresh({V1CertForNext(M + 15)}, {FreshRecordForNext(FreshFbForNext(G + 5))});
    const auto set0{Fresh()->current};
    const auto set1{Fresh()->next};
    BOOST_REQUIRE(set0 && set1);
    BOOST_CHECK(Fresh()->handover_certified);

    // V1 rotates at M+30; its epoch-1 handover (M+30) at M+33.
    ProduceTo(M + 32, m_vk_a);
    BOOST_CHECK_EQUAL(FinalityState().epoch, 1U);
    Produce(m_vk_a, {V1CertForNext(M + 30)});
    BOOST_CHECK(FinalityState().handover_certified);

    // The nominal fresh boundary R_f + E rotates (handover certified); not before.
    ProduceTo(Rf + E - 1, m_vk_a);
    BOOST_CHECK_EQUAL(Fresh()->epoch, 0U);
    BOOST_CHECK_EQUAL(ProjectedNext().fresh->epoch, 1U);
    const node::ValidatorSetSnapshot snap_before_rotation{IndependentSnapshotAtTip()}; // Snapshot(R_f + E - 1)
    Produce(m_vk_a); // R_f + E: rotation
    const int start1{Rf + E};
    {
        const auto fresh{Fresh()};
        BOOST_REQUIRE(fresh.has_value());
        BOOST_CHECK_EQUAL(fresh->epoch, 1U);
        BOOST_CHECK(fresh->epoch_starts == (std::vector<int>{Rf, start1}));
        BOOST_CHECK(!fresh->previous2); // no Set'_{-1}
        BOOST_CHECK_EQUAL(SetHashOf(fresh->previous).GetHex(), set0->set_hash.GetHex());
        BOOST_CHECK_EQUAL(SetHashOf(fresh->current).GetHex(), set1->set_hash.GetHex());
        BOOST_REQUIRE(fresh->next);
        BOOST_CHECK_EQUAL(fresh->next->Epoch(), 2U);
        BOOST_CHECK(fresh->next->header == modern::RulesetV2Header(snap_before_rotation.Header(), 2));
        BOOST_CHECK_EQUAL(fresh->next->Size(), 2U);
        BOOST_CHECK(!fresh->handover_certified);
        BOOST_CHECK_EQUAL(fresh->finalized->height, G + 5); // survives rotation
        BOOST_CHECK_EQUAL(SetHashOf(fresh->genesis).GetHex(), set0->set_hash.GetHex());
    }
    const auto set2{Fresh()->next};

    // Delayed epoch-0 certificate (checkpoint R_f + E - 1 = G + 30, index 6)
    // after the rotation: accepted, finalized advances, no epoch-1 handover.
    const int cp0{G + 30};
    BOOST_REQUIRE_EQUAL(cp0, start1 - 1);
    ProduceTo(cp0 + SCALED_DEPTH - 1, m_vk_a);
    const FinalizedBlockV2 carried_cp0{FreshFbForNext(cp0)};
    {
        const auto& fb{carried_cp0};
        BOOST_CHECK_EQUAL(fb.epoch, 0U);
        BOOST_CHECK_EQUAL(fb.checkpoint_index, 6U);
        BOOST_CHECK(fb.validator_set_hash == set1->set_hash); // Set'_0 attests Set'_1
        ProduceFresh({}, {FreshRecordForNext(fb)});
    }
    {
        const auto fresh{Fresh()};
        BOOST_CHECK_EQUAL(fresh->finalized->height, cp0);
        BOOST_CHECK_EQUAL(fresh->finalized->epoch, 0U);
        BOOST_CHECK_EQUAL(fresh->history.count, 7U);
        BOOST_CHECK(!fresh->handover_certified);
        BOOST_CHECK_EQUAL(fresh->epoch, 1U);
    }
    // Epoch relation: an epoch-1 object for cp0, which lies in fresh epoch 0.
    CheckRejected({FreshRecordForNext(FreshFbForNext(cp0, /*epoch=*/1))}, "fresh-epoch-relation");

    // Fresh epoch-1 handover (G+35, index 7) by Set'_1.
    const int cp1{G + 35};
    ProduceTo(cp1 + SCALED_DEPTH - 1, m_vk_a);
    ProduceFresh({}, {FreshRecordForNext(FreshFbForNext(cp1))});
    BOOST_CHECK(Fresh()->handover_certified);
    BOOST_CHECK_EQUAL(Fresh()->finalized->checkpoint_index, 7U);

    // B revokes its binding: later snapshots hold A alone (< MIN_FINALITY_SET).
    const auto revoke_b{MakeBinding(m_validator_b, m_vk_b, nullptr, 1)};
    Produce(m_vk_a, {}, {MakeTx(4, {revoke_b.cell}, {revoke_b.record})});
    BOOST_CHECK_EQUAL(IndependentSnapshotAtTip().Size(), 1U);

    // V1 rotates at M+60 (carried over too); its epoch-2 handover (M+60) at M+63.
    ProduceTo(M + 62, m_vk_a);
    BOOST_CHECK_EQUAL(FinalityState().epoch, 2U);
    Produce(m_vk_a, {V1CertForNext(M + 60)});
    BOOST_CHECK(FinalityState().handover_certified);

    // Fresh rotation into epoch 2 at start1 + E: Set'_3 is a carry-over of
    // Set'_2 (the snapshot has one member), re-stamped epoch 3, B still listed.
    ProduceTo(start1 + E - 1, m_vk_a);
    BOOST_CHECK_EQUAL(Fresh()->epoch, 1U);
    Produce(m_vk_a);
    const int start2{start1 + E};
    {
        const auto fresh{Fresh()};
        BOOST_CHECK_EQUAL(fresh->epoch, 2U);
        BOOST_CHECK(fresh->epoch_starts == (std::vector<int>{Rf, start1, start2}));
        // Set'_0 stays retained as Set'_{e-2} for the bridge V2 verifier
        // judgement, while the certificate paths keep {e, e-1}.
        BOOST_CHECK_EQUAL(SetHashOf(fresh->previous2).GetHex(), set0->set_hash.GetHex());
        BOOST_CHECK_EQUAL(SetHashOf(fresh->previous).GetHex(), set1->set_hash.GetHex());
        BOOST_CHECK_EQUAL(SetHashOf(fresh->current).GetHex(), set2->set_hash.GetHex());
        BOOST_REQUIRE(fresh->next);
        BOOST_CHECK(!fresh->SetForEpoch(0));
        BOOST_CHECK(!fresh->SetSize(0));
        BOOST_CHECK_EQUAL(SetHashOf(fresh->SetForEpoch(1)).GetHex(), set1->set_hash.GetHex());
        BOOST_CHECK_EQUAL(SetHashOf(fresh->SetForEpoch(2)).GetHex(), set2->set_hash.GetHex());
        BOOST_CHECK(!fresh->SetForEpoch(3));
        BOOST_CHECK(fresh->SuccessorHashForEpoch(0) == std::optional<uint256>{set1->set_hash});
        BOOST_CHECK(fresh->SuccessorHashForEpoch(1) == std::optional<uint256>{set2->set_hash});
        BOOST_CHECK(fresh->SuccessorHashForEpoch(2) == std::optional<uint256>{fresh->next->set_hash});
        BOOST_CHECK(!fresh->SuccessorHashForEpoch(3));
        BOOST_CHECK(fresh->SetHashForEpoch(0) == std::optional<uint256>{set0->set_hash});
        BOOST_CHECK(fresh->SetHashForEpoch(1) == std::optional<uint256>{set1->set_hash});
        BOOST_CHECK(fresh->SetHashForEpoch(2) == std::optional<uint256>{set2->set_hash});
        BOOST_CHECK(fresh->SetHashForEpoch(3) == std::optional<uint256>{fresh->next->set_hash});
        BOOST_CHECK(!fresh->SetHashForEpoch(4));
        // B3 still forms the canonical epoch-0 statement (E5 for a verifier
        // two epochs behind): exactly the delayed certificate carried above.
        const auto epoch0{node::ExpectedFinalizedBlockV2(0, cp0, *fresh, ChainHashFn(),
                                                         m_node.chainman->GetConsensus(), nullptr)};
        BOOST_REQUIRE(epoch0.has_value());
        BOOST_CHECK(*epoch0 == carried_cp0);
        BOOST_CHECK(modern::FinalityCertIdV2(*epoch0) == modern::FinalityCertIdV2(carried_cp0));
        BOOST_CHECK_EQUAL(fresh->next->Size(), 2U);
        BOOST_CHECK(fresh->next->members == set2->members); // the same members, shared
        BOOST_CHECK(fresh->next->header == modern::RulesetV2Header(set2->members->Header(), 3));
        for (size_t i{1}; i < fresh->epoch_starts.size(); ++i) {
            BOOST_CHECK(fresh->epoch_starts[i] > fresh->epoch_starts[i - 1]);
        }
    }
    // The carrier still serves {e, e-1} only: an epoch-0 certificate is
    // refused by the matcher although Set'_0 is retained.
    CheckRejected({FreshRecordForNext(carried_cp0)}, "fresh-cert-unknown-epoch-set");

    // No fresh epoch-2 handover: the fresh lineage breaks at start2 + E +
    // extension while V1 (handover certified, epoch 3 from M+90) continues.
    const int break_height{start2 + E + FRESH_EXTENSION};
    ProduceTo(break_height - 1, m_vk_a);
    BOOST_CHECK(!Fresh()->lineage_broken);
    BOOST_CHECK(ProjectedNext().fresh->lineage_broken);
    Produce(m_vk_a); // break_height
    {
        const auto s{FinalityState()};
        BOOST_REQUIRE(s.fresh.has_value());
        BOOST_CHECK(s.fresh->lineage_broken);
        BOOST_CHECK_EQUAL(s.fresh->epoch, 2U);
        BOOST_CHECK(!s.lineage_broken); // V1 untouched
        BOOST_CHECK_EQUAL(s.epoch, 3U);
        const auto view{node::MakeLineageV2View(m_node.chainman->GetConsensus(), s.fresh)};
        BOOST_CHECK(view.bootstrapped && view.broken && view.epoch == 2);
    }
    // A perfect epoch-2 V2 certificate is now refused; a V1 certificate is accepted.
    const int cp_late{G + 120};
    BOOST_REQUIRE(modern::IsCheckpointHeight(cp_late, M, SCALED_INTERVAL));
    ProduceTo(cp_late + SCALED_DEPTH - 1, m_vk_a);
    CheckRejected({FreshRecordForNext(FreshFbForNext(cp_late, std::nullopt, /*ignore_broken=*/true))},
                  "fresh-lineage-broken");
    ProduceFresh({V1CertForNext(cp_late)}, {});
    {
        const auto s{FinalityState()};
        BOOST_CHECK_EQUAL(s.finalized->height, cp_late);
        BOOST_CHECK(s.handover_certified);
        BOOST_CHECK(s.fresh->lineage_broken);
        BOOST_CHECK_EQUAL(s.fresh->finalized->height, cp1); // the fresh tip stays put
    }
    // Rebuild equality after everything above.
    const auto before{FinalityState()};
    WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().ModernFinality().MarkDirty());
    const auto after{FinalityState()};
    BOOST_CHECK(SameV1State(before, after));
    BOOST_CHECK(SameFreshState(before.fresh, after.fresh));
}

// ------------------------------------------------------ journal namespace
//
// The fresh lineage keeps its own signer journal per (lineage id, key)
// (finality-dependency section 6.4): its own file name and magic, never
// reading, resetting or deleting a V1 journal. These cases need no chain.

BOOST_FIXTURE_TEST_CASE(journal_namespaces_are_distinct_files_with_independent_locks, BasicTestingSetup)
{
    const fs::path dir{m_path_root / "fresh-signer-journals"};
    const uint256 domain{m_rng.rand256()};
    const uint256 lineage{m_rng.rand256()};
    modern::ValidatorKeyBytes vk{};
    vk.fill(0x5a);

    // Paths: the default stays the V1 name; the fresh name is distinct even
    // when both scope ids carry the same value.
    const fs::path v1_path{FinalitySignerStore::StatePath(dir, domain, vk)};
    BOOST_CHECK(v1_path == FinalitySignerStore::StatePath(dir, domain, vk, FinalitySignerNamespace::CHAIN_DOMAIN_V1));
    BOOST_CHECK_EQUAL(fs::PathToString(v1_path.filename()),
                      "finality_signer_v1_" + domain.GetHex() + "_" + HexStr(vk) + ".dat");
    const fs::path v2_path{FinalitySignerStore::StatePath(dir, lineage, vk, FinalitySignerNamespace::FRESH_LINEAGE_V2)};
    BOOST_CHECK_EQUAL(fs::PathToString(v2_path.filename()),
                      "finality_signer_v2_lineage_" + lineage.GetHex() + "_" + HexStr(vk) + ".dat");
    BOOST_CHECK(FinalitySignerStore::StatePath(dir, domain, vk) !=
                FinalitySignerStore::StatePath(dir, domain, vk, FinalitySignerNamespace::FRESH_LINEAGE_V2));
    // A value outside the enum has no path and cannot open.
    const auto bogus{static_cast<FinalitySignerNamespace>(3)};
    BOOST_CHECK(FinalitySignerStore::StatePath(dir, lineage, vk, bogus).empty());
    {
        FinalitySignerStore store;
        std::string error;
        BOOST_CHECK(!store.Open(dir, lineage, vk, error, bogus));
        BOOST_CHECK_EQUAL(error, "invalid finality signer store identity");
        BOOST_CHECK(!store.IsOpen());
    }

    std::string error;
    FinalitySignerStore v1;
    FinalitySignerStore v2;
    BOOST_REQUIRE_MESSAGE(v1.Open(dir, domain, vk, error), error);
    BOOST_REQUIRE_MESSAGE(v2.Open(dir, lineage, vk, error, FinalitySignerNamespace::FRESH_LINEAGE_V2), error);
    BOOST_CHECK(v1.Namespace() == FinalitySignerNamespace::CHAIN_DOMAIN_V1);
    BOOST_CHECK(v2.Namespace() == FinalitySignerNamespace::FRESH_LINEAGE_V2);
    BOOST_CHECK(v1.Path() == v1_path);
    BOOST_CHECK(v2.Path() == v2_path);
    BOOST_CHECK(v2.ScopeId() == lineage);
    BOOST_CHECK(v2.ChainDomain() == lineage);
    BOOST_CHECK(v1.IsAbsent() && v2.IsAbsent());

    // Initializing one journal does not create the other.
    BOOST_REQUIRE_MESSAGE(v1.InitializeEmpty(error), error);
    BOOST_CHECK(fs::exists(v1_path));
    BOOST_CHECK(!fs::exists(v2_path));
    BOOST_REQUIRE_MESSAGE(v2.InitializeEmpty(error), error);
    BOOST_CHECK_EQUAL(FileMagic(v1_path), "B3FSLOCK");
    BOOST_CHECK_EQUAL(FileMagic(v2_path), "B3FSLIN2");

    // A V1 vote moves only the V1 watermark and lock.
    const uint256 v1_block{m_rng.rand256()}, v1_digest{m_rng.rand256()};
    const uint256 v1_set{m_rng.rand256()}, v1_successor{m_rng.rand256()};
    BOOST_REQUIRE_MESSAGE(v1.CommitSignedCheckpoint(10, v1_block, v1_digest, 0, v1_set, v1_successor, error), error);
    BOOST_CHECK_EQUAL(v2.State()->last_signed_height, -1);
    BOOST_CHECK_EQUAL(v2.State()->lock_height, -1);

    // The fresh journal votes at the same height for a different block,
    // digest and (ruleset-2) set: no V1 rule applies to it.
    const uint256 v2_block{m_rng.rand256()}, v2_digest{m_rng.rand256()};
    const uint256 v2_set{m_rng.rand256()}, v2_successor{m_rng.rand256()};
    BOOST_REQUIRE_MESSAGE(v2.CommitSignedCheckpoint(10, v2_block, v2_digest, 0, v2_set, v2_successor, error), error);
    // A certified V2 anchor moves only the fresh lock.
    const uint256 v2_anchor_block{m_rng.rand256()}, v2_anchor_digest{m_rng.rand256()};
    BOOST_REQUIRE_MESSAGE(v2.CommitCertifiedAnchor(20, v2_anchor_block, v2_anchor_digest, 0, v2_set,
                                                   v2_successor, error), error);
    // ... and the V1 lock still refuses what the fresh lock allowed.
    BOOST_CHECK(!v1.CommitSignedCheckpoint(10, v2_block, v2_digest, 0, v1_set, v1_successor, error));
    BOOST_CHECK_EQUAL(error, "refusing a competing digest at an already signed height");

    // Each journal restores only its own record.
    FinalitySignerStore v1_restarted;
    FinalitySignerStore v2_restarted;
    BOOST_REQUIRE_MESSAGE(v1_restarted.Open(dir, domain, vk, error), error);
    BOOST_REQUIRE_MESSAGE(v2_restarted.Open(dir, lineage, vk, error, FinalitySignerNamespace::FRESH_LINEAGE_V2), error);
    BOOST_REQUIRE(v1_restarted.State() && v2_restarted.State());
    BOOST_CHECK(*v1_restarted.State() == *v1.State());
    BOOST_CHECK(*v2_restarted.State() == *v2.State());
    BOOST_CHECK(v1_restarted.State()->chain_domain == domain);
    BOOST_CHECK_EQUAL(v1_restarted.State()->lock_height, 10);
    BOOST_CHECK(v1_restarted.State()->lock_block_hash == v1_block);
    BOOST_CHECK(v2_restarted.State()->chain_domain == lineage); // the scope id
    BOOST_CHECK_EQUAL(v2_restarted.State()->last_signed_height, 10);
    BOOST_CHECK(v2_restarted.State()->last_signed_block_hash == v2_block);
    BOOST_CHECK_EQUAL(v2_restarted.State()->lock_height, 20);
    BOOST_CHECK(v2_restarted.State()->lock_block_hash == v2_anchor_block);

    // The recovery helpers stay V1-only. A record that exactly matches the
    // fresh journal's vote is refused by the fresh journal, while the same
    // shape is accepted by a V1 journal; the read-only preflight never reads
    // the fresh file.
    Consensus::FinalitySignerRecovery pin;
    pin.chain_domain = lineage;
    pin.validator_key = vk;
    pin.incident_height = 30;
    pin.incident_block_hash = m_rng.rand256();
    pin.incident_epoch = 0;
    pin.incident_signing_set_hash = v2_set;
    pin.incident_successor_set_hash = v2_successor;
    pin.anchor_height = 40;
    pin.anchor_block_hash = m_rng.rand256();
    BOOST_REQUIRE(pin.Valid());
    BOOST_REQUIRE_MESSAGE(v2_restarted.CommitSignedCheckpoint(30, pin.incident_block_hash, m_rng.rand256(), 0, v2_set,
                                                              v2_successor, error), error);
    BOOST_CHECK(!v2_restarted.CommitPinnedRecoveryAnchor(pin, m_rng.rand256(), error));
    BOOST_CHECK_EQUAL(error, "pinned recovery applies only to the V1 chain-domain journal");
    BOOST_CHECK_EQUAL(v2_restarted.State()->lock_height, 30);
    BOOST_CHECK(!FinalitySignerStore::CheckOperatorRecoveryIncident(dir, lineage, vk, pin, error));
    BOOST_CHECK_EQUAL(error, "operator-trusted recovery requires an intact existing signer journal; the journal is absent");
    {
        const fs::path v1_dir{m_path_root / "fresh-signer-journals-v1-pin"};
        FinalitySignerStore v1_pin;
        BOOST_REQUIRE_MESSAGE(v1_pin.Open(v1_dir, lineage, vk, error), error);
        BOOST_REQUIRE_MESSAGE(v1_pin.InitializeEmpty(error), error);
        BOOST_REQUIRE_MESSAGE(v1_pin.CommitSignedCheckpoint(30, pin.incident_block_hash, m_rng.rand256(), 0, v2_set,
                                                            v2_successor, error), error);
        BOOST_CHECK_MESSAGE(v1_pin.CommitPinnedRecoveryAnchor(pin, m_rng.rand256(), error), error);
        BOOST_CHECK_EQUAL(v1_pin.State()->lock_height, 40);
    }
}

BOOST_FIXTURE_TEST_CASE(journal_magic_and_scope_bind_the_namespace, BasicTestingSetup)
{
    const uint256 domain{m_rng.rand256()};
    const uint256 lineage{m_rng.rand256()};
    const uint256 other_lineage{m_rng.rand256()};
    modern::ValidatorKeyBytes vk{};
    vk.fill(0x6b);
    std::string error;

    const auto populate{[&](const fs::path& dir, const uint256& scope, FinalitySignerNamespace ns) {
        FinalitySignerStore store;
        BOOST_REQUIRE_MESSAGE(store.Open(dir, scope, vk, error, ns), error);
        BOOST_REQUIRE_MESSAGE(store.InitializeEmpty(error), error);
        BOOST_REQUIRE_MESSAGE(store.CommitSignedCheckpoint(15, m_rng.rand256(), m_rng.rand256(), 0, m_rng.rand256(),
                                                           m_rng.rand256(), error), error);
        return store.Path();
    }};

    // A V1 file copied to the V2 path fails on its magic. The same scope id
    // value is used so that only the magic can reject it.
    {
        const fs::path dir{m_path_root / "v1-as-v2"};
        const fs::path v1_path{populate(dir, lineage, FinalitySignerNamespace::CHAIN_DOMAIN_V1)};
        const auto v1_bytes{FileBytes(v1_path)};
        const fs::path v2_path{FinalitySignerStore::StatePath(dir, lineage, vk, FinalitySignerNamespace::FRESH_LINEAGE_V2)};
        BOOST_REQUIRE(fs::copy_file(v1_path, v2_path, fs::copy_options::overwrite_existing));
        FinalitySignerStore v2;
        BOOST_CHECK(!v2.Open(dir, lineage, vk, error, FinalitySignerNamespace::FRESH_LINEAGE_V2));
        BOOST_CHECK_EQUAL(error, "unsafe finality signer state: wrong signer-state file magic");
        BOOST_CHECK(!v2.IsOpen());
        BOOST_CHECK(FileBytes(v1_path) == v1_bytes);
        BOOST_CHECK(FileBytes(v2_path) == v1_bytes);
    }
    // ... and a V2 file copied to the V1 path fails the same way.
    {
        const fs::path dir{m_path_root / "v2-as-v1"};
        const fs::path v2_path{populate(dir, domain, FinalitySignerNamespace::FRESH_LINEAGE_V2)};
        const fs::path v1_path{FinalitySignerStore::StatePath(dir, domain, vk)};
        BOOST_REQUIRE(fs::copy_file(v2_path, v1_path, fs::copy_options::overwrite_existing));
        FinalitySignerStore v1;
        BOOST_CHECK(!v1.Open(dir, domain, vk, error));
        BOOST_CHECK_EQUAL(error, "unsafe finality signer state: wrong signer-state file magic");
    }
    // A journal that changes namespace under an open store is never
    // overwritten: the predecessor check reads it with this store's magic.
    {
        const fs::path dir{m_path_root / "swapped-under-open"};
        FinalitySignerStore v1;
        BOOST_REQUIRE_MESSAGE(v1.Open(dir, domain, vk, error), error);
        BOOST_REQUIRE_MESSAGE(v1.InitializeEmpty(error), error);
        const fs::path v2_path{populate(dir, domain, FinalitySignerNamespace::FRESH_LINEAGE_V2)};
        BOOST_REQUIRE(fs::copy_file(v2_path, v1.Path(), fs::copy_options::overwrite_existing));
        const auto swapped{FileBytes(v1.Path())};
        BOOST_CHECK(!v1.CommitSignedCheckpoint(20, m_rng.rand256(), m_rng.rand256(), 0, m_rng.rand256(),
                                               m_rng.rand256(), error));
        BOOST_CHECK_EQUAL(error, "refusing to replace unsafe signer state: wrong signer-state file magic");
        BOOST_CHECK(FileBytes(v1.Path()) == swapped);
    }
    // A V2 journal is bound to its lineage id: one opened under another
    // lineage is a different (absent) file, and a copy at that file's path
    // fails on the identity it holds.
    {
        const fs::path dir{m_path_root / "other-lineage"};
        const fs::path v2_path{populate(dir, lineage, FinalitySignerNamespace::FRESH_LINEAGE_V2)};
        const auto v2_bytes{FileBytes(v2_path)};
        FinalitySignerStore other;
        BOOST_REQUIRE_MESSAGE(other.Open(dir, other_lineage, vk, error, FinalitySignerNamespace::FRESH_LINEAGE_V2), error);
        BOOST_CHECK(other.IsAbsent());
        BOOST_CHECK(other.Path() != v2_path);
        BOOST_REQUIRE(fs::copy_file(v2_path, other.Path(), fs::copy_options::overwrite_existing));
        FinalitySignerStore copied;
        BOOST_CHECK(!copied.Open(dir, other_lineage, vk, error, FinalitySignerNamespace::FRESH_LINEAGE_V2));
        BOOST_CHECK_EQUAL(error, "finality signer state belongs to another chain or validator");
        BOOST_CHECK(FileBytes(v2_path) == v2_bytes);
    }
}

BOOST_FIXTURE_TEST_CASE(v1_orphan_lock_does_not_block_fresh_lineage_votes, BasicTestingSetup)
{
    // RT4 item 2 (finality-dependency section 8), journal level: a validator
    // whose V1 journal is locked on an orphaned checkpoint still records a
    // fresh-lineage vote for the replacement branch, and the V1 journal is
    // neither read, reset nor rewritten by it. The signer-level case (the V1
    // signer refuses while the staging signer signs) is exercised with the
    // staging signer.
    const fs::path dir{m_path_root / "orphan-lock-journals"};
    const uint256 domain{m_rng.rand256()};
    const uint256 lineage{m_rng.rand256()};
    modern::ValidatorKeyBytes vk{};
    vk.fill(0x7c);
    std::string error;

    const int orphan_height{100};
    const uint256 orphan_block{m_rng.rand256()}, orphan_digest{m_rng.rand256()};
    const uint256 v1_set{m_rng.rand256()}, v1_successor{m_rng.rand256()};
    FinalitySignerStore v1;
    BOOST_REQUIRE_MESSAGE(v1.Open(dir, domain, vk, error), error);
    BOOST_REQUIRE_MESSAGE(v1.InitializeEmpty(error), error);
    BOOST_REQUIRE_MESSAGE(v1.CommitSignedCheckpoint(orphan_height, orphan_block, orphan_digest, 3, v1_set,
                                                    v1_successor, error), error);
    const auto v1_bytes{FileBytes(v1.Path())};

    // The replacement branch's checkpoint at the orphan height, and anything
    // lower, is refused by V1; a certificate of another set cannot unlock it.
    const uint256 replacement_block{m_rng.rand256()};
    BOOST_CHECK(!v1.CommitSignedCheckpoint(orphan_height, replacement_block, m_rng.rand256(), 3, v1_set,
                                           v1_successor, error));
    BOOST_CHECK(!v1.CommitSignedCheckpoint(orphan_height - 5, m_rng.rand256(), m_rng.rand256(), 3, v1_set,
                                           v1_successor, error));
    BOOST_CHECK(!v1.CommitCertifiedAnchor(orphan_height + 5, m_rng.rand256(), m_rng.rand256(), 0,
                                          m_rng.rand256(), m_rng.rand256(), error));

    // The fresh journal for the same validator signs the replacement branch
    // at and above the orphan height.
    const uint256 fresh_set{m_rng.rand256()}, fresh_successor{m_rng.rand256()};
    FinalitySignerStore v2;
    BOOST_REQUIRE_MESSAGE(v2.Open(dir, lineage, vk, error, FinalitySignerNamespace::FRESH_LINEAGE_V2), error);
    BOOST_CHECK(v2.IsAbsent());
    BOOST_REQUIRE_MESSAGE(v2.InitializeEmpty(error), error);
    BOOST_REQUIRE_MESSAGE(v2.CommitSignedCheckpoint(orphan_height, replacement_block, m_rng.rand256(), 0, fresh_set,
                                                    fresh_successor, error), error);
    const uint256 next_block{m_rng.rand256()};
    BOOST_REQUIRE_MESSAGE(v2.CommitSignedCheckpoint(orphan_height + 5, next_block, m_rng.rand256(), 0, fresh_set,
                                                    fresh_successor, error), error);
    BOOST_CHECK_EQUAL(v2.State()->lock_height, orphan_height + 5);
    BOOST_CHECK(v2.State()->lock_block_hash == next_block);

    // The V1 journal is byte-identical and still holds its orphan lock.
    BOOST_CHECK(FileBytes(v1.Path()) == v1_bytes);
    FinalitySignerStore v1_restarted;
    BOOST_REQUIRE_MESSAGE(v1_restarted.Open(dir, domain, vk, error), error);
    BOOST_REQUIRE(v1_restarted.State());
    BOOST_CHECK_EQUAL(v1_restarted.State()->lock_height, orphan_height);
    BOOST_CHECK(v1_restarted.State()->lock_block_hash == orphan_block);
    BOOST_CHECK(!v1_restarted.CommitSignedCheckpoint(orphan_height, replacement_block, m_rng.rand256(), 3, v1_set,
                                                     v1_successor, error));
}

// ------------------------------------------------ vote pool and staging signer

BOOST_FIXTURE_TEST_CASE(fresh_pool_verifies_dedupes_and_aggregates_into_a_consensus_certificate, FreshLineageFixture)
{
    PrepareFresh();
    const int G{m_G};
    const Consensus::Params& consensus{m_node.chainman->GetConsensus()};
    FreshLineageSignaturePool pool;

    // The fixed 148-byte vote message: lineage id, then little-endian
    // integers exactly like the V1 `finsig`, then the signature.
    FreshLineageSig layout;
    for (size_t i{0}; i < 32; ++i) layout.lineage_id.begin()[i] = static_cast<unsigned char>(i + 1);
    layout.epoch = 0x0102030405060708;
    layout.height = 0x1122334455667788;
    layout.index = 0xa1b2c3d4;
    layout.signature.fill(0x5c);
    {
        DataStream ss;
        ss << layout;
        BOOST_REQUIRE_EQUAL(ss.size(), FreshLineageSig::SERIALIZED_SIZE);
        BOOST_REQUIRE_EQUAL(ss.size(), 148U);
        const std::vector<unsigned char> bytes(UCharCast(ss.data()), UCharCast(ss.data()) + ss.size());
        BOOST_CHECK(std::equal(bytes.begin(), bytes.begin() + 32, layout.lineage_id.begin()));
        BOOST_CHECK_EQUAL(bytes[32], 0x08);
        BOOST_CHECK_EQUAL(bytes[39], 0x01);
        BOOST_CHECK_EQUAL(bytes[40], 0x88);
        BOOST_CHECK_EQUAL(bytes[47], 0x11);
        BOOST_CHECK_EQUAL(bytes[48], 0xd4);
        BOOST_CHECK_EQUAL(bytes[51], 0xa1);
        BOOST_CHECK(std::all_of(bytes.begin() + 52, bytes.end(), [](unsigned char b) { return b == 0x5c; }));
        FreshLineageSig back;
        ss >> back;
        BOOST_CHECK(ss.empty());
        BOOST_CHECK(back == layout);
    }
    // No fresh lineage yet (tip M-1): nothing is accepted.
    BOOST_CHECK_EQUAL(AcceptName(SubmitFresh(pool, layout)), "unconfigured");

    ProduceTo(G + 7, m_vk_a);
    const FinalizedBlockV2 good{FreshFbAtTip(G + 5)};
    BOOST_CHECK(good == FreshFbForNext(G + 5));
    const FreshLineageSig vote_a{FreshVote(m_bls_a, m_vk_a, good)};
    const FreshLineageSig vote_b{FreshVote(m_bls_b, m_vk_b, good)};
    BOOST_CHECK(vote_a.index != vote_b.index);
    // Signable only once the tip is CHECKPOINT_DEPTH above the checkpoint.
    BOOST_CHECK_EQUAL(AcceptName(SubmitFresh(pool, vote_a)), "too-shallow");
    Produce(m_vk_a); // G+8
    BOOST_CHECK_EQUAL(pool.TrackedCheckpoints(), 0U);

    // Cheap rejections, each before any BLS work or slot.
    {
        Consensus::Params no_knob{consensus};
        no_knob.fresh_lineage.reset();
        BOOST_CHECK_EQUAL(AcceptName(SubmitFresh(pool, vote_a, &no_knob)), "unconfigured");
        auto v{vote_a};
        v.lineage_id = uint256::ONE;
        BOOST_CHECK_EQUAL(AcceptName(SubmitFresh(pool, v)), "lineage-mismatch");
        v = vote_a;
        v.height = G + 6; // off the fresh grid
        BOOST_CHECK_EQUAL(AcceptName(SubmitFresh(pool, v)), "not-checkpoint");
        v.height = G; // GENESIS_HEIGHT: history leaf 0, never signed
        BOOST_CHECK_EQUAL(AcceptName(SubmitFresh(pool, v)), "not-checkpoint");
        v.height = G + 10; // above the tip
        BOOST_CHECK_EQUAL(AcceptName(SubmitFresh(pool, v)), "not-checkpoint");
        v = vote_a;
        v.epoch = 1; // Set'_1 is not in force yet
        BOOST_CHECK_EQUAL(AcceptName(SubmitFresh(pool, v)), "unknown-epoch");
        v = vote_a;
        v.index = 2; // n = 2
        BOOST_CHECK_EQUAL(AcceptName(SubmitFresh(pool, v)), "bad-index");
        BOOST_CHECK_EQUAL(pool.TrackedCheckpoints(), 0U);
    }
    // BLS rejections: a V1-tagged signature over the analogous V1 object,
    // another member's key at A's index, and garbage.
    {
        modern::FinalizedBlock v1_fb;
        v1_fb.height = good.height;
        v1_fb.block_hash = good.block_hash;
        v1_fb.withdrawal_root = good.withdrawal_root;
        v1_fb.validator_set_hash = good.validator_set_hash;
        v1_fb.epoch = good.epoch;
        BOOST_CHECK_EQUAL(
            AcceptName(SubmitFresh(pool, FreshVote(m_bls_a, m_vk_a, good, modern::FinalityDigest(m_domain, v1_fb)))),
            "bad-signature");
        auto v{vote_b};
        v.index = vote_a.index;
        BOOST_CHECK_EQUAL(AcceptName(SubmitFresh(pool, v)), "bad-signature");
        v = vote_a;
        v.signature.fill(0x11);
        BOOST_CHECK_EQUAL(AcceptName(SubmitFresh(pool, v)), "bad-signature");
        BOOST_CHECK_EQUAL(pool.TrackedCheckpoints(), 0U);
    }
    // A chain whose fresh lineage id differs (another R_f over the same
    // blocks, judged by a tracker of its own): every retained slot is dropped
    // and the old lineage's vote no longer matches. A separate pool keeps the
    // main pool for the aggregation below; no type-11 carrier exists yet, so
    // the other tracker syncs over the same blocks.
    {
        FreshLineageSignaturePool other_pool;
        BOOST_CHECK_EQUAL(AcceptName(SubmitFresh(other_pool, vote_a)), "accepted");
        BOOST_CHECK_EQUAL(other_pool.TrackedCheckpoints(), 1U);
        LOCK(cs_main);
        Consensus::Params other{consensus};
        other.fresh_lineage->activation_height = m_Rf + SCALED_INTERVAL;
        FinalityTracker shadow;
        const CChain& chain{m_node.chainman->ActiveChain()};
        BOOST_REQUIRE(shadow.Sync(chain, m_node.chainman->m_blockman, other, *chain.Tip()));
        BOOST_REQUIRE(shadow.Current().fresh && shadow.Current().fresh->bootstrapped);
        const uint256 other_lineage{shadow.Current().fresh->lineage_id};
        BOOST_REQUIRE(other_lineage != vote_a.lineage_id);
        BOOST_CHECK_EQUAL(AcceptName(other_pool.Submit(vote_a, shadow, chain, other)), "lineage-mismatch");
        BOOST_CHECK_EQUAL(other_pool.TrackedCheckpoints(), 0U);
        BOOST_CHECK(other_pool.LineageId() == other_lineage);
        BOOST_CHECK(other_pool.VerifiedSignatures(Finality(), chain, consensus).empty());
        BOOST_CHECK(!other_pool.BestCertificate(Finality(), chain, consensus).has_value());
    }
    // A's vote: accepted once; the exact bytes again are a duplicate; other
    // bytes at a verified index never replace the record.
    BOOST_CHECK_EQUAL(AcceptName(SubmitFresh(pool, vote_a)), "accepted");
    BOOST_CHECK_EQUAL(AcceptName(SubmitFresh(pool, vote_a)), "duplicate");
    {
        auto v{vote_a};
        v.signature = vote_b.signature;
        BOOST_CHECK_EQUAL(AcceptName(SubmitFresh(pool, v)), "bad-signature");
    }
    BOOST_CHECK_EQUAL(pool.SignatureCount(0, G + 5), 1U);
    BOOST_CHECK(pool.LineageId() == good.lineage_id);
    {
        // A alone holds the weight quorum (15 of 16) but not the headcount (2 of 2).
        LOCK(cs_main);
        BOOST_CHECK(!pool.BestCertificate(Finality(), m_node.chainman->ActiveChain(), consensus).has_value());
    }
    BOOST_CHECK_EQUAL(AcceptName(SubmitFresh(pool, vote_b)), "accepted");
    BOOST_CHECK_EQUAL(pool.SignatureCount(0, G + 5), 2U);

    std::optional<std::pair<FinalizedBlockV2, modern::FinalityCertificate>> best;
    {
        LOCK(cs_main);
        const FinalityTracker& tracker{Finality()};
        const CChain& chain{m_node.chainman->ActiveChain()};
        const auto verified{pool.VerifiedSignatures(tracker, chain, consensus)};
        BOOST_REQUIRE_EQUAL(verified.size(), 2U);
        BOOST_CHECK(std::find(verified.begin(), verified.end(), vote_a) != verified.end());
        BOOST_CHECK(std::find(verified.begin(), verified.end(), vote_b) != verified.end());
        BOOST_CHECK(pool.BestCertificate(tracker, chain, consensus).has_value());
        best = pool.BestCertificate(tracker, chain, consensus, nullptr, chain.Tip());
        BOOST_REQUIRE(best.has_value());
        BOOST_CHECK(best->first == good);
        BOOST_CHECK(best->second.signer_bitmap == std::vector<unsigned char>{0x03});
        std::string error;
        BOOST_CHECK_MESSAGE(tracker.JudgeCandidateCertificateV2(best->first, best->second, *chain.Tip(), consensus,
                                                                error),
                            error);

        // The assembly hook appends the record in canonical order next to
        // an existing (here: V1-typed) coinbase record, and only with the knob.
        CMutableTransaction coinbase;
        CMpaRecord v1_like;
        v1_like.payload_type = modern::MPA_TYPE_FINALITY_CERTIFICATE;
        v1_like.payload_version = modern::MPA_VERSION_V1;
        v1_like.payload = {0x01};
        coinbase.mpa = {v1_like};
        Consensus::Params no_knob{consensus};
        no_knob.fresh_lineage.reset();
        BOOST_CHECK(!node::AppendFreshLineageCertificate(tracker, pool, chain, no_knob, *chain.Tip(), nullptr, coinbase));
        BOOST_CHECK_EQUAL(coinbase.mpa.size(), 1U);
        BOOST_REQUIRE(node::AppendFreshLineageCertificate(tracker, pool, chain, consensus, *chain.Tip(), nullptr,
                                                          coinbase));
        BOOST_REQUIRE_EQUAL(coinbase.mpa.size(), 2U);
        BOOST_CHECK_EQUAL(coinbase.mpa[0].payload_type, modern::MPA_TYPE_FINALITY_CERTIFICATE);
        BOOST_CHECK_EQUAL(coinbase.mpa[1].payload_type, modern::MPA_TYPE_FINALITY_CERTIFICATE_V2);
        BOOST_CHECK_EQUAL(coinbase.mpa[1].payload_version, modern::MPA_VERSION_V1);
        BOOST_CHECK(coinbase.mpa[1].payload == modern::EncodeCertificatePayloadV2(best->first, best->second));
    }

    // The aggregated certificate is accepted by consensus in a block.
    CMpaRecord record;
    record.payload_type = modern::MPA_TYPE_FINALITY_CERTIFICATE_V2;
    record.payload_version = modern::MPA_VERSION_V1;
    record.payload = modern::EncodeCertificatePayloadV2(best->first, best->second);
    ProduceFresh({}, {record}); // G+9
    {
        const auto fresh{Fresh()};
        BOOST_REQUIRE(fresh && fresh->finalized);
        BOOST_CHECK_EQUAL(fresh->finalized->height, G + 5);
        BOOST_CHECK_EQUAL(fresh->finalized->certified_at, G + 9);
        BOOST_CHECK_EQUAL(fresh->finalized->cert_id.GetHex(), modern::FinalityCertIdV2(good).GetHex());
    }
    // Finalized slots are pruned and their votes are stale.
    BOOST_CHECK_EQUAL(AcceptName(SubmitFresh(pool, vote_b)), "stale");
    BOOST_CHECK_EQUAL(pool.TrackedCheckpoints(), 0U);

    // The export RPCs on the same chain: the included certificate through
    // the RPCs (status and Verifier V2 calldata export), then the hidden vote
    // relay into the node's own pool, whose certificate exports against the
    // previous fresh-finalized index by default.
    LineageRpc rpc{m_node};
    const auto hash_at{ChainHashFn()};
    const uint256 hash_g{*hash_at(G)};
    const uint256 leaf0{modern::FinalityHistoryLeaf(G, hash_g)};
    const uint256 leaf1{modern::FinalityHistoryLeaf(G + 5, *hash_at(G + 5))};
    {
        const auto fresh{Fresh()};
        BOOST_REQUIRE(fresh && fresh->bootstrapped && fresh->genesis && fresh->current && fresh->next);
        const UniValue status{rpc.Call("getfinalitylineage")};
        BOOST_CHECK(status["configured"].get_bool());
        BOOST_CHECK(status["staging_signer"].get_bool());
        BOOST_CHECK_EQUAL(status["activation_height"].getInt<int>(), m_Rf);
        BOOST_CHECK_EQUAL(status["genesis_height"].getInt<int>(), G);
        BOOST_CHECK_EQUAL(status["max_epoch_extension"].getInt<int>(), FRESH_EXTENSION);
        BOOST_CHECK_EQUAL(status["checkpoint_interval"].getInt<int>(), SCALED_INTERVAL);
        BOOST_CHECK_EQUAL(status["chain_domain"].get_str(), Evm0x(m_domain));
        BOOST_CHECK_EQUAL(status["tip_height"].getInt<int>(), G + 9);
        BOOST_CHECK(status["state_available"].get_bool());
        BOOST_CHECK(status["bootstrapped"].get_bool());
        BOOST_CHECK_EQUAL(status["genesis_block_hash"].get_str(), hash_g.GetHex());
        BOOST_CHECK_EQUAL(status["genesis_block_hash_evm"].get_str(), Evm0x(hash_g));
        BOOST_CHECK_EQUAL(status["genesis_leaf"].get_str(), Evm0x(leaf0));
        // The deployer's pins recompute to the exported lineage id.
        const uint256 genesis_set_hash{FromEvm0x(status["genesis_set_hash"].get_str())};
        BOOST_CHECK(genesis_set_hash == fresh->genesis->set_hash);
        BOOST_CHECK_EQUAL(status["lineage_id"].get_str(),
                          Evm0x(modern::FinalityLineageIdV2(m_domain, G, hash_g, genesis_set_hash)));
        BOOST_CHECK_EQUAL(status["lineage_id"].get_str(), Evm0x(good.lineage_id));
        BOOST_CHECK_EQUAL(status["epoch"].getInt<int>(), 0);
        BOOST_CHECK_EQUAL(status["epoch_starts"].write(), strprintf("[%d]", m_Rf));
        BOOST_CHECK(status["handover_certified"].get_bool()); // the epoch-0 certificate is included
        BOOST_CHECK(!status["lineage_broken"].get_bool());
        BOOST_CHECK_EQUAL(status["break_height"].getInt<int>(), m_Rf + SCALED_E + FRESH_EXTENSION);
        BOOST_CHECK(status["previous_set"].isNull());
        for (const char* key : {"genesis_set", "current_set", "next_set"}) {
            const UniValue& set{status[key]};
            BOOST_CHECK_EQUAL(set["ruleset_version"].getInt<int>(), 2);
            BOOST_CHECK_EQUAL(set["validator_count"].getInt<int>(), 2);
            const auto encoded{ParseHex(set["encoded_header"].get_str().substr(2))};
            BOOST_CHECK_EQUAL(encoded.size(), 110U);
            BOOST_CHECK_EQUAL(Evm0x(modern::Keccak(encoded)), set["set_hash"].get_str());
            BOOST_CHECK_EQUAL(set["members_root"].get_str(), Evm0x(fresh->current->header.members_root));
        }
        BOOST_CHECK_EQUAL(status["current_set"]["set_hash"].get_str(), Evm0x(fresh->current->set_hash));
        BOOST_CHECK_EQUAL(status["next_set"]["epoch"].getInt<int>(), 1);
        BOOST_CHECK_EQUAL(status["next_set"]["set_hash"].get_str(), Evm0x(fresh->next->set_hash));
        const UniValue& fin{status["finalized"]};
        BOOST_CHECK_EQUAL(fin["height"].getInt<int>(), G + 5);
        BOOST_CHECK_EQUAL(fin["block_hash_evm"].get_str(), Evm0x(good.block_hash));
        BOOST_CHECK_EQUAL(fin["checkpoint_index"].getInt<int>(), 1);
        BOOST_CHECK_EQUAL(fin["checkpoint_history_root"].get_str(), Evm0x(good.checkpoint_history_root));
        BOOST_CHECK_EQUAL(fin["cert_id"].get_str(), Evm0x(modern::FinalityCertIdV2(good)));
        BOOST_CHECK_EQUAL(fin["certified_at"].getInt<int>(), G + 9);
        const UniValue& hist{status["history"]};
        BOOST_CHECK_EQUAL(hist["checkpoint_count"].getInt<int>(), 2);
        BOOST_CHECK_EQUAL(hist["last_checkpoint_height"].getInt<int>(), G + 5);
        BOOST_CHECK_EQUAL(hist["finalized_leaf_count"].getInt<int>(), 2);
        const std::vector<uint256> leaves01{leaf0, leaf1};
        BOOST_CHECK_EQUAL(hist["root"].get_str(), Evm0x(*modern::FinalityHistoryRoot(leaves01)));
        BOOST_CHECK_EQUAL(status["pool_checkpoints"].getInt<int>(), 0);
        // No signer ran into this node's pool: no local signer status.
        BOOST_CHECK(status["local_signer"].isNull());

        // The certificate block, by height and by hash.
        const std::string at{strprintf("%d", G + 9)};
        const UniValue cert{rpc.Call("getfinalitylineagecertificate", {at})};
        BOOST_CHECK_EQUAL(cert.write(), rpc.Call("getfinalitylineagecertificate", {hash_at(G + 9)->GetHex()}).write());
        BOOST_CHECK_EQUAL(cert["inclusion_height"].getInt<int>(), G + 9);
        BOOST_CHECK_EQUAL(cert["inclusion_block"].get_str(), hash_at(G + 9)->GetHex());
        BOOST_CHECK_EQUAL(cert["lineage_id"].get_str(), Evm0x(good.lineage_id));
        BOOST_CHECK_EQUAL(cert["height"].getInt<int>(), G + 5);
        BOOST_CHECK_EQUAL(cert["block_hash"].get_str(), good.block_hash.GetHex());
        BOOST_CHECK_EQUAL(cert["block_hash_evm"].get_str(), Evm0x(good.block_hash));
        BOOST_CHECK_EQUAL(cert["withdrawal_root"].get_str(), Evm0x(uint256{}));
        BOOST_CHECK_EQUAL(cert["validator_set_hash"].get_str(), Evm0x(fresh->next->set_hash));
        BOOST_CHECK_EQUAL(cert["epoch"].getInt<int>(), 0);
        BOOST_CHECK_EQUAL(cert["checkpoint_index"].getInt<int>(), 1);
        BOOST_CHECK_EQUAL(cert["checkpoint_history_root"].get_str(), Evm0x(good.checkpoint_history_root));
        BOOST_CHECK_EQUAL(cert["bridge_flags"].getInt<int>(), 0);
        BOOST_CHECK_EQUAL(cert["encoded"].get_str(), Evm0x(good.Encode()));
        BOOST_CHECK_EQUAL(cert["digest_v2"].get_str(), Evm0x(modern::FinalityDigestV2(m_domain, good)));
        BOOST_CHECK_EQUAL(cert["cert_id"].get_str(), Evm0x(modern::FinalityCertIdV2(good)));
        BOOST_CHECK_EQUAL(cert["signing_set"].write(), status["current_set"].write());
        BOOST_CHECK_EQUAL(cert["successor_set"].write(), status["next_set"].write());
        BOOST_CHECK_EQUAL(cert["validator_count"].getInt<int>(), 2);
        BOOST_CHECK_EQUAL(cert["signer_bitmap"].get_str(), "0x03");
        BOOST_CHECK_EQUAL(cert["signer_count"].getInt<int>(), 2);
        BOOST_CHECK(cert["absent_indices"].empty());
        const UniValue& h{cert["history"]};
        BOOST_CHECK_EQUAL(h["prev_index"].getInt<int>(), 0);
        BOOST_CHECK_EQUAL(h["prev_height"].getInt<int>(), G);
        BOOST_CHECK_EQUAL(h["prev_block_hash"].get_str(), hash_g.GetHex());
        BOOST_CHECK_EQUAL(h["prev_leaf"].get_str(), Evm0x(leaf0));
        BOOST_CHECK_EQUAL(h["genesis_height"].getInt<int>(), G);
        BOOST_CHECK_EQUAL(h["genesis_leaf"].get_str(), Evm0x(leaf0));
        BOOST_CHECK_EQUAL(h["ancestry_path"].write(), h["leaf0_path"].write());
        BOOST_CHECK(modern::FoldFinalityHistoryPath(leaf0, 0, PathFromJSON(h["ancestry_path"])) ==
                    good.checkpoint_history_root);
        // Exactly the export builder's bytes for the same objects.
        std::string error;
        const std::vector<uint256> leaves0{leaf0};
        const auto direct{node::BuildFreshLineageRelayArtifacts(m_domain, good, best->second, *fresh->current,
                                                                *fresh->next, leaves0, 0, error)};
        BOOST_REQUIRE_MESSAGE(direct, error);
        BOOST_CHECK_EQUAL(cert["proof"].get_str(), Evm0x(direct->proof_abi));
        BOOST_CHECK_EQUAL(cert["calldata"].get_str(), Evm0x(direct->submit_calldata));
        BOOST_CHECK(cert["calldata"].get_str().starts_with("0x6263c1cb"));
        BOOST_CHECK_EQUAL(cert["function"].get_str(), std::string{node::FRESH_SUBMIT_CERTIFICATE_SIGNATURE});
        BOOST_CHECK(!cert["private_key_required_by_rpc"].get_bool());

        // Fail closed: index range, a block without a record, below R_f,
        // above the tip and a negative index.
        BOOST_CHECK(Contains(rpc.Error("getfinalitylineagecertificate", {at, "1"}),
                             "prev_index 1 must be below the certificate's checkpoint index 1"));
        BOOST_CHECK(Contains(rpc.Error("getfinalitylineagecertificate", {strprintf("%d", G + 8)}),
                             "contains no fresh-lineage certificate"));
        BOOST_CHECK(Contains(rpc.Error("getfinalitylineagecertificate", {strprintf("%d", G)}),
                             "below the fresh lineage activation height"));
        BOOST_CHECK(Contains(rpc.Error("getfinalitylineagecertificate", {strprintf("%d", G + 10)}),
                             "after current tip"));
        BOOST_CHECK(Contains(rpc.Error("getfinalitylineagecertificate", {at, "-1"}), "nonnegative"));
    }

    // Hidden vote relay: the node's pool is empty; a finalized vote is stale;
    // only exact 148-byte messages are parsed.
    BOOST_CHECK(rpc.Call("getfinalitylineagevotes").empty());
    BOOST_CHECK_EQUAL(rpc.Call("submitfinalitylineagevote", {VoteHex(vote_b)})["result"].get_str(), "stale");
    BOOST_CHECK(Contains(rpc.Error("submitfinalitylineagevote", {VoteHex(vote_b).substr(2)}), "exactly 148 bytes"));
    BOOST_CHECK(Contains(rpc.Error("submitfinalitylineagevote", {"abcd"}), "exactly 148 bytes"));
    BOOST_CHECK(!rpc.Error("submitfinalitylineagevote", {"zz"}).empty());

    ProduceTo(G + 13, m_vk_a);
    const FinalizedBlockV2 second{FreshFbAtTip(G + 10)};
    BOOST_CHECK_EQUAL(second.checkpoint_index, 2U);
    const FreshLineageSig a2{FreshVote(m_bls_a, m_vk_a, second)};
    const FreshLineageSig b2{FreshVote(m_bls_b, m_vk_b, second)};
    BOOST_CHECK_EQUAL(rpc.Call("submitfinalitylineagevote", {VoteHex(a2)})["result"].get_str(), "accepted");
    BOOST_CHECK_EQUAL(rpc.Call("submitfinalitylineagevote", {VoteHex(a2)})["result"].get_str(), "duplicate");
    {
        const UniValue votes{rpc.Call("getfinalitylineagevotes")};
        BOOST_REQUIRE_EQUAL(votes.size(), 1U);
        BOOST_CHECK_EQUAL(votes[0]["hex"].get_str(), VoteHex(a2));
        BOOST_CHECK_EQUAL(votes[0]["lineage_id"].get_str(), Evm0x(good.lineage_id));
        BOOST_CHECK_EQUAL(votes[0]["epoch"].getInt<int>(), 0);
        BOOST_CHECK_EQUAL(votes[0]["height"].getInt<int>(), G + 10);
        BOOST_CHECK_EQUAL(votes[0]["index"].getInt<int>(), static_cast<int>(a2.index));
        BOOST_CHECK_EQUAL(votes[0]["signature"].get_str(), HexStr(a2.signature));
        BOOST_CHECK_EQUAL(rpc.Call("getfinalitylineage")["pool_checkpoints"].getInt<int>(), 1);
    }
    BOOST_CHECK_EQUAL(rpc.Call("submitfinalitylineagevote", {VoteHex(b2)})["result"].get_str(), "accepted");
    std::optional<std::pair<FinalizedBlockV2, modern::FinalityCertificate>> best2;
    {
        LOCK(cs_main);
        Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
        best2 = chainstate.FinalitySignatures().Fresh().BestCertificate(Finality(), chainstate.m_chain, consensus,
                                                                        nullptr, chainstate.m_chain.Tip());
    }
    BOOST_REQUIRE(best2.has_value());
    BOOST_CHECK(best2->first == second);
    CMpaRecord record2;
    record2.payload_type = modern::MPA_TYPE_FINALITY_CERTIFICATE_V2;
    record2.payload_version = modern::MPA_VERSION_V1;
    record2.payload = modern::EncodeCertificatePayloadV2(best2->first, best2->second);
    ProduceFresh({}, {record2}); // G+14
    {
        const std::string at{strprintf("%d", G + 14)};
        const UniValue cert{rpc.Call("getfinalitylineagecertificate", {at})};
        BOOST_CHECK_EQUAL(cert["height"].getInt<int>(), G + 10);
        BOOST_CHECK_EQUAL(cert["checkpoint_index"].getInt<int>(), 2);
        const UniValue& h{cert["history"]};
        // Default: the previous fresh-finalized checkpoint (G+5, index 1).
        BOOST_CHECK_EQUAL(h["prev_index"].getInt<int>(), 1);
        BOOST_CHECK_EQUAL(h["prev_height"].getInt<int>(), G + 5);
        BOOST_CHECK_EQUAL(h["prev_leaf"].get_str(), Evm0x(leaf1));
        BOOST_CHECK(h["ancestry_path"].write() != h["leaf0_path"].write());
        BOOST_CHECK(modern::FoldFinalityHistoryPath(leaf1, 1, PathFromJSON(h["ancestry_path"])) ==
                    second.checkpoint_history_root);
        BOOST_CHECK(modern::FoldFinalityHistoryPath(leaf0, 0, PathFromJSON(h["leaf0_path"])) ==
                    second.checkpoint_history_root);
        // ancestryPath sits in head words 11..42 of the calldata.
        const auto calldata{ParseHex(cert["calldata"].get_str().substr(2))};
        const auto path{PathFromJSON(h["ancestry_path"])};
        for (size_t i{0}; i < path.size(); ++i) BOOST_CHECK(WordIs(ArgWord(calldata, 11 + i), path[i]));
        // An explicit index 0 serves a verifier that holds only the genesis leaf.
        const UniValue cert0{rpc.Call("getfinalitylineagecertificate", {at, "0"})};
        BOOST_CHECK_EQUAL(cert0["history"]["ancestry_path"].write(), h["leaf0_path"].write());
        BOOST_CHECK(cert0["calldata"].get_str() != cert["calldata"].get_str());
        BOOST_CHECK_EQUAL(cert0["proof"].get_str(), cert["proof"].get_str());
        BOOST_CHECK(Contains(rpc.Error("getfinalitylineagecertificate", {at, "2"}),
                             "prev_index 2 must be below the certificate's checkpoint index 2"));
        const UniValue status{rpc.Call("getfinalitylineage")};
        BOOST_CHECK_EQUAL(status["finalized"]["height"].getInt<int>(), G + 10);
        BOOST_CHECK_EQUAL(status["finalized"]["checkpoint_index"].getInt<int>(), 2);
        BOOST_CHECK_EQUAL(status["history"]["finalized_leaf_count"].getInt<int>(), 3);
        // As in V1's pool, the finalized slot is reclaimed on the next
        // submission; its votes are no longer exported meanwhile.
        BOOST_CHECK(rpc.Call("getfinalitylineagevotes").empty());
        BOOST_CHECK_EQUAL(status["pool_checkpoints"].getInt<int>(), 1);
        BOOST_CHECK_EQUAL(rpc.Call("submitfinalitylineagevote", {VoteHex(a2)})["result"].get_str(), "stale");
        BOOST_CHECK_EQUAL(rpc.Call("getfinalitylineage")["pool_checkpoints"].getInt<int>(), 0);
    }
}

BOOST_FIXTURE_TEST_CASE(staging_signer_journals_v1_equivalence_orphan_lock_and_assembler, FreshLineageFixture)
{
    PrepareFresh();
    const int M{m_M};
    const int G{m_G};
    const Consensus::Params& consensus{m_node.chainman->GetConsensus()};
    Consensus::Params v1_only{consensus};
    v1_only.fresh_lineage.reset();
    Consensus::Params staging_off{consensus};
    staging_off.fresh_lineage->staging_signer = false;

    const fs::path dir_knob{m_path_root / "signer-knob"};
    const fs::path dir_plain{m_path_root / "signer-plain"};
    const fs::path dir_off{m_path_root / "signer-staging-off"};
    const fs::path dir_orphan{m_path_root / "signer-orphan"};
    const fs::path dir_late{m_path_root / "signer-late"};

    // B's V1 journal holds an ancestry lock on a checkpoint that is not on
    // this chain (an orphaned vote): its V1 signer refuses throughout.
    const int orphan_height{M + 5};
    {
        FinalitySignerStore store;
        std::string error;
        BOOST_REQUIRE_MESSAGE(store.Open(dir_orphan, m_domain, m_vk_b, error), error);
        BOOST_REQUIRE_MESSAGE(store.InitializeEmpty(error), error);
        BOOST_REQUIRE_MESSAGE(store.CommitSignedCheckpoint(orphan_height, m_rng.rand256(), m_rng.rand256(), 0,
                                                           m_rng.rand256(), m_rng.rand256(), error),
                              error);
    }
    const fs::path orphan_v1_path{FinalitySignerStore::StatePath(dir_orphan, m_domain, m_vk_b)};
    const auto orphan_v1_bytes{FileBytes(orphan_v1_path)};

    std::string error;
    node::FinalitySigner knob_a;   // A, knob on: chainstate pool
    node::FinalitySigner plain_a;  // A, the same chain judged without the knob
    node::FinalitySigner off_a;    // A, knob on but staging_signer = false
    node::FinalitySigner orphan_b; // B with the V1 orphan lock: chainstate pool
    BOOST_REQUIRE_MESSAGE(knob_a.SetKeysPersistent({m_bls_a}, m_vk_a, m_domain, dir_knob, error), error);
    BOOST_REQUIRE_MESSAGE(plain_a.SetKeysPersistent({m_bls_a}, m_vk_a, m_domain, dir_plain, error), error);
    BOOST_REQUIRE_MESSAGE(off_a.SetKeysPersistent({m_bls_a}, m_vk_a, m_domain, dir_off, error), error);
    BOOST_REQUIRE_MESSAGE(orphan_b.SetKeysPersistent({m_bls_b}, m_vk_b, m_domain, dir_orphan, error), error);
    node::FinalitySignaturePool plain_pool;
    node::FinalitySignaturePool off_pool;
    FinalityTracker shadow; // synced with v1_only

    // One staking-loop pass of every signer at the current tip; the V1
    // results of the knob, no-knob and staging-off signers must be equal.
    const auto sign_all{[&]() {
        LOCK(cs_main);
        Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
        const CChain& chain{chainstate.m_chain};
        FinalityTracker& tracker{Finality()};
        BOOST_REQUIRE(shadow.Sync(chain, m_node.chainman->m_blockman, v1_only, *chain.Tip()));
        BOOST_CHECK(!shadow.Current().fresh.has_value());
        const auto knob{knob_a.MaybeSign(tracker, chain, consensus, chainstate.FinalitySignatures())};
        const auto plain{plain_a.MaybeSign(shadow, chain, v1_only, plain_pool)};
        const auto off{off_a.MaybeSign(tracker, chain, staging_off, off_pool)};
        BOOST_CHECK(knob == plain);
        BOOST_CHECK(knob == off);
        BOOST_CHECK_EQUAL(knob_a.LastSignedHeight(), plain_a.LastSignedHeight());
        BOOST_CHECK_EQUAL(knob_a.LastError(), plain_a.LastError());
        BOOST_CHECK(knob_a.LastError().empty());
        // The V1 signer of B refuses on its orphan lock (non-permanent).
        BOOST_CHECK(orphan_b.MaybeSign(tracker, chain, consensus, chainstate.FinalitySignatures()).empty());
        BOOST_CHECK_MESSAGE(orphan_b.LastError().find("does not descend from signed checkpoint") != std::string::npos,
                            orphan_b.LastError());
        return knob;
    }};

    // The first pass at M-1 establishes the empty V1 journals before any V1
    // checkpoint is signable, as a staking loop started before M does.
    BOOST_CHECK(sign_all().empty());
    // Below R_f - 1 nothing fresh exists: V1 only, no fresh journal anywhere.
    ProduceTo(G - 1, m_vk_a);
    BOOST_CHECK(!sign_all().empty()); // V1 checkpoint M+5
    for (const auto& dir : {dir_knob, dir_plain, dir_off, dir_orphan}) BOOST_CHECK_EQUAL(FreshJournalCount(dir), 0U);

    // At R_f - 1 the projection for R_f names the lineage: the staging
    // signers establish their empty fresh journals before anything is signable.
    Produce(m_vk_a); // G
    const auto projected{ProjectedNext().fresh};
    BOOST_REQUIRE(projected && projected->bootstrapped);
    const uint256 lineage_id{projected->lineage_id};
    sign_all();
    const fs::path knob_v2_path{
        FinalitySignerStore::StatePath(dir_knob, lineage_id, m_vk_a, FinalitySignerNamespace::FRESH_LINEAGE_V2)};
    const fs::path orphan_v2_path{
        FinalitySignerStore::StatePath(dir_orphan, lineage_id, m_vk_b, FinalitySignerNamespace::FRESH_LINEAGE_V2)};
    BOOST_CHECK(fs::exists(knob_v2_path));
    BOOST_CHECK(fs::exists(orphan_v2_path));
    BOOST_CHECK_EQUAL(FileMagic(knob_v2_path), "B3FSLIN2");
    BOOST_CHECK(knob_a.FreshStore().IsOpen() && knob_a.FreshStore().State());
    BOOST_CHECK(knob_a.FreshStore().ScopeId() == lineage_id);
    BOOST_CHECK_EQUAL(knob_a.FreshStore().State()->last_signed_height, -1);
    BOOST_CHECK_EQUAL(FreshJournalCount(dir_plain), 0U);
    BOOST_CHECK_EQUAL(FreshJournalCount(dir_off), 0U);

    // G+5 becomes signable at G+8: the staging signers record and pool their
    // fresh votes; V1 results are unchanged; B signs V2 despite its V1 lock.
    ProduceTo(G + 8, m_vk_a);
    BOOST_CHECK(!sign_all().empty()); // V1 checkpoints M+10, M+15
    const FinalizedBlockV2 expected{FreshFbAtTip(G + 5)};
    BOOST_CHECK(expected.lineage_id == lineage_id);
    const uint256 expected_digest{modern::FinalityDigestV2(m_domain, expected)};
    const auto fresh_now{Fresh()};
    BOOST_REQUIRE(fresh_now && fresh_now->current && fresh_now->next);
    for (const auto* signer : {&knob_a, &orphan_b}) {
        BOOST_CHECK_EQUAL(signer->FreshLastSignedHeight(), G + 5);
        BOOST_CHECK_MESSAGE(signer->FreshLastError().empty(), signer->FreshLastError());
        BOOST_REQUIRE(signer->FreshStore().State());
        const auto& journal{*signer->FreshStore().State()};
        BOOST_CHECK(journal.chain_domain == lineage_id); // scope id
        BOOST_CHECK_EQUAL(journal.last_signed_height, G + 5);
        BOOST_CHECK(journal.last_signed_block_hash == ChainHashAt(G + 5));
        BOOST_CHECK_EQUAL(journal.last_signed_digest.GetHex(), expected_digest.GetHex());
        BOOST_CHECK_EQUAL(journal.lock_height, G + 5);
        BOOST_CHECK_EQUAL(journal.lock_epoch, 0U);
        BOOST_CHECK(journal.lock_signing_set_hash == fresh_now->current->set_hash); // ruleset 2
        BOOST_CHECK(journal.lock_successor_set_hash == fresh_now->next->set_hash);
    }
    BOOST_CHECK(orphan_b.LastSignedHeight() == orphan_height); // V1 watermark untouched
    BOOST_CHECK(FileBytes(orphan_v1_path) == orphan_v1_bytes);  // V1 journal byte-identical
    // The V1 journals of the knob and no-knob signers are byte-identical.
    BOOST_CHECK(FileBytes(FinalitySignerStore::StatePath(dir_knob, m_domain, m_vk_a)) ==
                FileBytes(FinalitySignerStore::StatePath(dir_plain, m_domain, m_vk_a)));
    // No fresh journal or fresh vote without the knob or with staging off.
    BOOST_CHECK_EQUAL(FreshJournalCount(dir_plain), 0U);
    BOOST_CHECK_EQUAL(FreshJournalCount(dir_off), 0U);
    BOOST_CHECK_EQUAL(plain_pool.Fresh().TrackedCheckpoints(), 0U);
    BOOST_CHECK_EQUAL(off_pool.Fresh().TrackedCheckpoints(), 0U);
    BOOST_CHECK_EQUAL(off_a.FreshLastSignedHeight(), -1);
    {
        LOCK(cs_main);
        const auto& fresh_pool{m_node.chainman->ActiveChainstate().FinalitySignatures().Fresh()};
        BOOST_CHECK_EQUAL(fresh_pool.SignatureCount(0, G + 5), 2U);
        BOOST_CHECK(fresh_pool.LineageId() == lineage_id);
    }
    // Each journal is reloaded on restart: no repeat vote, same watermark.
    {
        node::FinalitySigner restarted;
        BOOST_REQUIRE_MESSAGE(restarted.SetKeysPersistent({m_bls_a}, m_vk_a, m_domain, dir_knob, error), error);
        LOCK(cs_main);
        Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
        node::FinalitySignaturePool scratch;
        BOOST_CHECK(restarted.MaybeSignFresh(Finality(), chainstate.m_chain, consensus, scratch.Fresh()).empty());
        BOOST_CHECK_EQUAL(restarted.FreshLastSignedHeight(), G + 5);
        BOOST_CHECK(restarted.FreshLastError().empty());
    }
    // A validator whose fresh journal is missing after a fresh checkpoint of
    // its committee became signable refuses to recreate it (not permanent).
    {
        node::FinalitySigner late;
        BOOST_REQUIRE_MESSAGE(late.SetKeysPersistent({m_bls_a}, m_vk_a, m_domain, dir_late, error), error);
        LOCK(cs_main);
        Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
        node::FinalitySignaturePool scratch;
        BOOST_CHECK(late.MaybeSignFresh(Finality(), chainstate.m_chain, consensus, scratch.Fresh()).empty());
        BOOST_CHECK_MESSAGE(late.FreshLastError().find("fresh-lineage signer journal is absent") != std::string::npos,
                            late.FreshLastError());
        BOOST_CHECK(late.FreshStore().IsAbsent());
        BOOST_CHECK(!fs::exists(late.FreshStore().Path()));
        // The refusal and the watermark reach the pool's node-local status.
        const auto& status{scratch.Fresh().LocalSigner()};
        BOOST_REQUIRE(status.has_value());
        BOOST_CHECK_EQUAL(status->last_signed_height, -1);
        BOOST_CHECK_EQUAL(status->last_error, late.FreshLastError());
    }
    // V1's absent-journal rule, unchanged: before the first included fresh
    // certificate even a validator outside Set'_0 (C, never staked here)
    // refuses to create its journal once a fresh checkpoint is signable.
    const fs::path dir_newcomer{m_path_root / "signer-newcomer"};
    {
        node::FinalitySigner newcomer;
        BOOST_REQUIRE_MESSAGE(newcomer.SetKeysPersistent({m_bls_c}, m_vk_c, m_domain, dir_newcomer, error), error);
        LOCK(cs_main);
        Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
        BOOST_REQUIRE(!Finality().Current().fresh->current->IndexOf(m_vk_c).has_value());
        node::FinalitySignaturePool scratch;
        BOOST_CHECK(newcomer.MaybeSignFresh(Finality(), chainstate.m_chain, consensus, scratch.Fresh()).empty());
        BOOST_CHECK_MESSAGE(newcomer.FreshLastError().find(strprintf(
                                "fresh-lineage signer journal is absent after fresh checkpoint %d", G + 5)) !=
                                std::string::npos,
                            newcomer.FreshLastError());
        BOOST_CHECK(newcomer.FreshStore().IsAbsent());
    }
    // The staging signers reported into the chainstate pool; the signers
    // without the knob or with staging off reported nothing.
    BOOST_CHECK(!plain_pool.Fresh().LocalSigner().has_value());
    BOOST_CHECK(!off_pool.Fresh().LocalSigner().has_value());
    {
        LineageRpc rpc{m_node};
        const UniValue local{rpc.Call("getfinalitylineage")["local_signer"]};
        BOOST_REQUIRE(local.isObject());
        BOOST_CHECK_EQUAL(local["last_signed_height"].getInt<int>(), G + 5);
        BOOST_CHECK_EQUAL(local["last_error"].get_str(), "");
    }

    // node::BlockAssembler includes the judged type-11 record (no V1 quorum
    // exists: B's V1 signer refuses), the block connects and finalizes G+5.
    node::BlockAssembler::Options options;
    options.coinbase_output_script = CScript() << OP_TRUE;
    options.include_dummy_extranonce = true;
    options.modern_pos_validator_key = m_vk_a;
    ChainstateManager& chainman{*m_node.chainman};
    const auto tmpl{node::BlockAssembler(chainman.ActiveChainstate(), nullptr, options).CreateNewBlock()};
    BOOST_REQUIRE(tmpl);
    CBlock block{tmpl->block};
    {
        const CTransaction& cb{*block.vtx[0]};
        BOOST_REQUIRE_EQUAL(cb.mpa.size(), 1U);
        BOOST_CHECK_EQUAL(cb.mpa[0].payload_type, modern::MPA_TYPE_FINALITY_CERTIFICATE_V2);
        const auto state{ProjectedNext()};
        std::optional<modern::FinalityCertificateV2Record> record;
        BOOST_REQUIRE_MESSAGE(modern::MatchFinalityCertificateV2ForEpoch(
                                  cb, [&state](const uint64_t e) { return state.fresh->SetSize(e); }, record, error),
                              error);
        BOOST_REQUIRE(record.has_value());
        BOOST_CHECK(record->finalized_block == expected);
        BOOST_CHECK(record->certificate.signer_bitmap == std::vector<unsigned char>{0x03});
        BOOST_CHECK(!tmpl->m_coinbase_tx.mpa_section.empty());
        std::optional<modern::FinalityCertificatePair> v1_pair;
        BOOST_REQUIRE_MESSAGE(modern::MatchFinalityCertificate(cb, 2, v1_pair, error), error);
        BOOST_CHECK(!v1_pair.has_value());
    }
    block.hashMerkleRoot = BlockMerkleRoot(block);
    Sign(block, m_validator_a);
    if (block.GetBlockTime() > GetTime()) SetMockTime(block.GetBlockTime());
    BOOST_REQUIRE_MESSAGE(Probe(block).empty(), Probe(block));
    // No V1 certificate exists on this chain, so nothing is pinned yet.
    BOOST_CHECK(!WITH_LOCK(cs_main, return m_node.chainman->m_blockman.FinalityAnchor()).has_value());
    BOOST_REQUIRE(Submit(block));
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, G + 9);
    {
        const auto s{FinalityState()};
        BOOST_REQUIRE(s.fresh && s.fresh->finalized);
        BOOST_CHECK_EQUAL(s.fresh->finalized->height, G + 5);
        BOOST_CHECK_EQUAL(s.fresh->finalized->certified_at, G + 9);
        BOOST_CHECK(s.fresh->handover_certified);
        BOOST_CHECK(!s.finalized.has_value()); // no V1 certificate was ever formed
    }
    // The included V2 certificate pins G+5 exactly as a V1 certificate would
    // (the V1-broken case of regtest fixture B): a fork below it is refused
    // at the header level, and the anchor is re-derived identically.
    {
        const auto anchor{WITH_LOCK(cs_main, return m_node.chainman->m_blockman.FinalityAnchor())};
        BOOST_REQUIRE(anchor.has_value());
        BOOST_CHECK_EQUAL(anchor->first, G + 5);
        BOOST_CHECK_EQUAL(anchor->second.GetHex(), ChainHashAt(G + 5).GetHex());
        BOOST_CHECK(WITH_LOCK(cs_main, return m_node.chainman->ActiveChainstate().ModernFinality().PinnedCheckpoint()) ==
                    std::make_optional(std::make_pair(G + 5, ChainHashAt(G + 5))));
        for (const int parent_height : {G + 4, G + 1}) {
            const CBlockIndex* parent{IndexAt(parent_height)};
            const auto [side, digest] = BuildPosBlockOnSeed(parent, SeedFor(parent), m_vk_a, {}, {},
                                                            /*extra=*/parent_height);
            BOOST_CHECK(!Submit(side));
            BOOST_CHECK(WITH_LOCK(cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(side.GetHash())) ==
                        nullptr);
            BOOST_CHECK_EQUAL(Tip()->nHeight, G + 9);
        }
        BOOST_CHECK(WITH_LOCK(cs_main, return m_node.chainman->ActiveChainstate().RefreshFinalityAnchor()));
        BOOST_CHECK(WITH_LOCK(cs_main, return m_node.chainman->m_blockman.FinalityAnchor()) == anchor);
    }
    // Once a fresh certificate is included, a validator seated in no
    // unfinalized window checkpoint (C) may establish its empty journal.
    {
        node::FinalitySigner newcomer;
        BOOST_REQUIRE_MESSAGE(newcomer.SetKeysPersistent({m_bls_c}, m_vk_c, m_domain, dir_newcomer, error), error);
        LOCK(cs_main);
        Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
        node::FinalitySignaturePool scratch;
        BOOST_CHECK(newcomer.MaybeSignFresh(Finality(), chainstate.m_chain, consensus, scratch.Fresh()).empty());
        BOOST_CHECK_MESSAGE(newcomer.FreshLastError().empty(), newcomer.FreshLastError());
        BOOST_REQUIRE(newcomer.FreshStore().State());
        BOOST_CHECK_EQUAL(newcomer.FreshStore().State()->last_signed_height, -1);
        BOOST_CHECK(fs::exists(newcomer.FreshStore().Path()));
    }
    // A repeat template does not re-include the finalized checkpoint.
    {
        const auto tmpl2{node::BlockAssembler(chainman.ActiveChainstate(), nullptr, options).CreateNewBlock()};
        BOOST_REQUIRE(tmpl2);
        BOOST_CHECK(!tmpl2->block.vtx[0]->HasMpa());
    }

    // A fresh vote orphaned below the included fresh certificate (lock at
    // G+3 on a block not on this chain): the lock moves to the certified
    // G+5 only when the orphaned lock used the same fresh epoch and
    // committee; the recorded vote is retained. Otherwise the signer refuses
    // (not permanently). There is no pinned or operator recovery here.
    const auto populate_orphan{[&](const fs::path& dir, const uint256& signing_set_hash) {
        FinalitySignerStore store;
        BOOST_REQUIRE_MESSAGE(store.Open(dir, lineage_id, m_vk_a, error, FinalitySignerNamespace::FRESH_LINEAGE_V2),
                              error);
        BOOST_REQUIRE_MESSAGE(store.InitializeEmpty(error), error);
        BOOST_REQUIRE_MESSAGE(store.CommitSignedCheckpoint(G + 3, m_rng.rand256(), m_rng.rand256(), 0,
                                                           signing_set_hash, fresh_now->next->set_hash, error),
                              error);
    }};
    const fs::path dir_moved{m_path_root / "signer-fresh-orphan-same-set"};
    const fs::path dir_foreign{m_path_root / "signer-fresh-orphan-other-set"};
    populate_orphan(dir_moved, fresh_now->current->set_hash);
    populate_orphan(dir_foreign, m_rng.rand256());
    {
        node::FinalitySigner moved;
        BOOST_REQUIRE_MESSAGE(moved.SetKeysPersistent({m_bls_a}, m_vk_a, m_domain, dir_moved, error), error);
        node::FinalitySigner foreign;
        BOOST_REQUIRE_MESSAGE(foreign.SetKeysPersistent({m_bls_a}, m_vk_a, m_domain, dir_foreign, error), error);
        LOCK(cs_main);
        Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
        node::FinalitySignaturePool scratch;
        BOOST_CHECK(moved.MaybeSignFresh(Finality(), chainstate.m_chain, consensus, scratch.Fresh()).empty());
        BOOST_CHECK_MESSAGE(moved.FreshLastError().empty(), moved.FreshLastError());
        BOOST_REQUIRE(moved.FreshStore().State());
        const auto& journal{*moved.FreshStore().State()};
        BOOST_CHECK_EQUAL(journal.last_signed_height, G + 3); // the vote is retained
        BOOST_CHECK_EQUAL(journal.lock_height, G + 5);
        BOOST_CHECK(journal.lock_block_hash == ChainHashAt(G + 5));
        BOOST_CHECK_EQUAL(journal.lock_digest.GetHex(), expected_digest.GetHex());
        BOOST_CHECK(journal.lock_signing_set_hash == fresh_now->current->set_hash);

        BOOST_CHECK(foreign.MaybeSignFresh(Finality(), chainstate.m_chain, consensus, scratch.Fresh()).empty());
        BOOST_CHECK_MESSAGE(foreign.FreshLastError().find("does not use the exact fresh epoch and committee") !=
                                std::string::npos,
                            foreign.FreshLastError());
        BOOST_REQUIRE(foreign.FreshStore().State());
        BOOST_CHECK_EQUAL(foreign.FreshStore().State()->lock_height, G + 3);
    }
}

// ---------------------------------------------------------- Verifier V2 export

BOOST_FIXTURE_TEST_CASE(relay_artifacts_v2_calldata_layout_paths_and_refusals, BasicTestingSetup)
{
    const uint256 domain{Asym(0xd0)};
    const ExportMembers set{{5, 6, 7, 8}}; // W = 26, quorum floor(52/3)+1 = 18, headcount 3
    const auto signing{node::MakeFreshLineageSet(set.snapshot, 3)};
    const auto successor{node::MakeFreshLineageSet(set.snapshot, 4)};
    BOOST_REQUIRE(signing && successor);
    BOOST_REQUIRE_EQUAL(signing->header.quorum_weight, 18U);
    BOOST_REQUIRE_EQUAL(signing->header.ruleset_version, modern::FINALITY_RULESET_V2);

    // Five history leaves at G + 5i (G = 261, contracts-spec vector grid);
    // the certificate is index 5 at height 286.
    constexpr uint64_t G{261};
    constexpr uint64_t INTERVAL{5};
    constexpr uint64_t K{5};
    std::vector<uint256> leaves;
    for (uint64_t i{0}; i < K; ++i) {
        leaves.push_back(modern::FinalityHistoryLeaf(G + i * INTERVAL, Asym(static_cast<unsigned char>(0x40 + i))));
    }
    FinalizedBlockV2 fb;
    fb.lineage_id = Asym(0x10);
    fb.height = G + K * INTERVAL;
    fb.block_hash = Asym(0x20);
    fb.withdrawal_root = uint256{};
    fb.validator_set_hash = successor->set_hash;
    fb.epoch = 3;
    fb.checkpoint_index = K;
    fb.checkpoint_history_root = *modern::FinalityHistoryRoot(leaves);
    fb.bridge_flags = 0;
    const modern::FinalityCertificate cert{set.Sign(modern::FinalityDigestV2(domain, fb), {0, 1, 3})};

    std::string error;
    const auto a{node::BuildFreshLineageRelayArtifacts(domain, fb, cert, *signing, *successor, leaves, 2, error)};
    BOOST_REQUIRE_MESSAGE(a, error);
    const std::span<const unsigned char> cd{a->submit_calldata};
    BOOST_TEST_MESSAGE("fresh-lineage submitCertificate calldata: 0x" << HexStr(cd));

    // Selector = keccak256(signature)[0:4]: 0x6263c1cb from `cast sig` (1.7.1)
    // and from `forge inspect --offline` of contracts/src/v2/
    // IB3FinalityVerifierV2.sol (methodIdentifiers).
    const std::string_view signature{node::FRESH_SUBMIT_CERTIFICATE_SIGNATURE};
    const uint256 signature_hash{modern::Keccak(std::span<const unsigned char>(
        reinterpret_cast<const unsigned char*>(signature.data()), signature.size()))};
    BOOST_CHECK(std::equal(cd.begin(), cd.begin() + 4, signature_hash.begin()));
    BOOST_CHECK_EQUAL(HexStr(cd.first(4)), "6263c1cb");
    // Whole-call vector (BLS signing is deterministic): `cast decode-calldata`
    // (1.7.1) decodes these bytes against the signature and `cast calldata`
    // re-encodes the decoded arguments byte-identically.
    BOOST_CHECK_EQUAL(cd.size(), 3588U);
    BOOST_CHECK_EQUAL(HexStr(modern::Keccak(cd)), "3b3db0f86a7fe183ef2c442bddd9f2e343bd0871700e0172a4a04dcb8ea89431");

    // Head words 0..8: the FinalizedBlockV2 fields in contracts-spec order.
    BOOST_CHECK(WordIs(ArgWord(cd, 0), fb.lineage_id));
    BOOST_CHECK_EQUAL(ArgU64(cd, 1), fb.height);
    BOOST_CHECK(WordIs(ArgWord(cd, 2), fb.block_hash));
    BOOST_CHECK(WordIs(ArgWord(cd, 3), fb.withdrawal_root));
    BOOST_CHECK(WordIs(ArgWord(cd, 4), fb.validator_set_hash));
    BOOST_CHECK_EQUAL(ArgU64(cd, 5), fb.epoch);
    BOOST_CHECK_EQUAL(ArgU64(cd, 6), fb.checkpoint_index);
    BOOST_CHECK(WordIs(ArgWord(cd, 7), fb.checkpoint_history_root));
    BOOST_CHECK_EQUAL(ArgU64(cd, 8), uint64_t{fb.bridge_flags});

    // Offsets 9, 10 and 43 point at the two ruleset-2 SetHeader tuples and
    // the proof bytes, which fill the tail exactly.
    const auto signing_tail{bridge::bootstrap_detail::EncodeSetHeaderTuple(signing->header)};
    const auto successor_tail{bridge::bootstrap_detail::EncodeSetHeaderTuple(successor->header)};
    const auto proof_tail{bridge::bootstrap_detail::DynamicBytes(a->proof_abi)};
    constexpr uint64_t HEAD{node::FRESH_SUBMIT_HEAD_WORDS * 32};
    BOOST_CHECK_EQUAL(HEAD, 44U * 32U);
    BOOST_CHECK_EQUAL(ArgU64(cd, 9), HEAD);
    BOOST_CHECK_EQUAL(ArgU64(cd, 10), HEAD + signing_tail.size());
    BOOST_CHECK_EQUAL(ArgU64(cd, 43), HEAD + signing_tail.size() + successor_tail.size());
    BOOST_REQUIRE_EQUAL(cd.size(), 4 + HEAD + signing_tail.size() + successor_tail.size() + proof_tail.size());
    const auto args{cd.subspan(4)};
    BOOST_CHECK(std::ranges::equal(args.subspan(ArgU64(cd, 9), signing_tail.size()), signing_tail));
    BOOST_CHECK(std::ranges::equal(args.subspan(ArgU64(cd, 10), successor_tail.size()), successor_tail));
    BOOST_CHECK(std::ranges::equal(args.subspan(ArgU64(cd, 43)), proof_tail));
    // Tuple word 0 is the fresh epoch, word 1 the ruleset.
    BOOST_CHECK_EQUAL(ReadBE64(args.data() + ArgU64(cd, 9) + 24), 3U);
    BOOST_CHECK_EQUAL(ReadBE64(args.data() + ArgU64(cd, 9) + 32 + 24), 2U);
    BOOST_CHECK_EQUAL(ReadBE64(args.data() + ArgU64(cd, 10) + 24), 4U);
    BOOST_CHECK_EQUAL(ReadBE64(args.data() + ArgU64(cd, 10) + 32 + 24), 2U);
    BOOST_CHECK(modern::ValidatorSetHash(a->signing_header) == signing->set_hash);
    BOOST_CHECK(modern::ValidatorSetHash(a->successor_header) == fb.validator_set_hash);
    // Head words 11..42: ancestryPath.
    for (size_t i{0}; i < modern::FINALITY_HISTORY_DEPTH; ++i) {
        BOOST_CHECK(WordIs(ArgWord(cd, 11 + i), a->ancestry_path[i]));
    }

    // proof_abi is EncodeProof over the same set and bitmap; the absent
    // member's witness is rebuilt here from the snapshot.
    BOOST_CHECK_EQUAL(HexStr(a->bitmap), "0b");
    BOOST_CHECK_EQUAL(a->signer_count, 3U);
    const auto& members{set.snapshot->Members()};
    BOOST_CHECK_EQUAL(a->signed_weight, members[0].weight + members[1].weight + members[3].weight);
    BOOST_REQUIRE_EQUAL(a->absent.size(), 1U);
    BOOST_CHECK_EQUAL(a->absent[0].index, 2U);
    bridge::BootstrapAbsentWitness absent;
    absent.index = 2;
    absent.compressed_pubkey = members[2].bls_pubkey;
    absent.weight = members[2].weight;
    absent.uncompressed_pubkey = bls::PublicKey::Decode(members[2].bls_pubkey)->Eip2537Uncompressed();
    uint256 members_root;
    absent.siblings = bridge::bootstrap_detail::MerklePath(set.snapshot->Leaves(), 2, members_root);
    BOOST_CHECK(members_root == signing->header.members_root);
    const auto expected_proof{bridge::bootstrap_detail::EncodeProof(
        cert.signer_bitmap, bls::Signature::Decode(cert.aggregate_sig)->Eip2537Uncompressed(),
        bls::PublicKey::Decode(signing->header.aggregate_pubkey)->Eip2537Uncompressed(), absent)};
    BOOST_CHECK(a->proof_abi == expected_proof);

    // Both paths fold to the certificate's history root, for every prev index.
    BOOST_CHECK(a->prev_leaf == leaves[2]);
    BOOST_CHECK(modern::FoldFinalityHistoryPath(leaves[2], 2, a->ancestry_path) == fb.checkpoint_history_root);
    BOOST_CHECK(a->leaf0 == leaves[0]);
    BOOST_CHECK(modern::FoldFinalityHistoryPath(leaves[0], 0, a->leaf0_path) == fb.checkpoint_history_root);
    for (uint64_t prev{0}; prev < K; ++prev) {
        const auto b{node::BuildFreshLineageRelayArtifacts(domain, fb, cert, *signing, *successor, leaves, prev, error)};
        BOOST_REQUIRE_MESSAGE(b, error);
        BOOST_CHECK_EQUAL(b->prev_index, prev);
        BOOST_CHECK(modern::FoldFinalityHistoryPath(leaves[prev], prev, b->ancestry_path) ==
                    fb.checkpoint_history_root);
        BOOST_CHECK(b->leaf0_path == a->leaf0_path);
        BOOST_CHECK(b->proof_abi == a->proof_abi);
        if (prev == 0) BOOST_CHECK(b->ancestry_path == b->leaf0_path);
        if (prev != 2) BOOST_CHECK(b->submit_calldata != a->submit_calldata);
    }
    // Bridge flag bit 0 lands in head word 8.
    {
        FinalizedBlockV2 closing{fb};
        closing.bridge_flags = modern::FINALITY_BRIDGE_FLAG_DEPOSITS_CLOSED;
        const auto c{node::BuildFreshLineageRelayArtifacts(
            domain, closing, set.Sign(modern::FinalityDigestV2(domain, closing), {0, 1, 3}), *signing, *successor,
            leaves, 2, error)};
        BOOST_REQUIRE_MESSAGE(c, error);
        BOOST_CHECK_EQUAL(ArgU64(c->submit_calldata, 8), 1U);
    }

    // Refusals, each with its reason.
    const auto refused{[&](const FinalizedBlockV2& f, const modern::FinalityCertificate& c,
                           const node::FreshLineageSet& s, const node::FreshLineageSet& succ,
                           std::span<const uint256> l, const uint64_t prev, const uint256& d) {
        std::string e;
        BOOST_CHECK(!node::BuildFreshLineageRelayArtifacts(d, f, c, s, succ, l, prev, e).has_value());
        return e;
    }};
    const std::span<const uint256> all{leaves};
    BOOST_CHECK(Contains(refused(fb, cert, *signing, *successor, all, K, domain), "prev_index must be below"));
    BOOST_CHECK(Contains(refused(fb, cert, *signing, *successor, all, K + 3, domain), "prev_index must be below"));
    BOOST_CHECK(Contains(refused(fb, cert, *signing, *successor, all.first(K - 1), 0, domain), "exactly leaves"));
    {
        auto bad{leaves};
        bad[3] = Asym(0x99);
        BOOST_CHECK(Contains(refused(fb, cert, *signing, *successor, bad, 2, domain), "do not reproduce"));
    }
    BOOST_CHECK(Contains(refused(fb, cert, *signing, *signing, all, 2, domain), "successor-committee epoch"));
    BOOST_CHECK(Contains(refused(fb, cert, *node::MakeFreshLineageSet(set.snapshot, 2), *successor, all, 2, domain),
                         "signing-committee epoch"));
    {
        const ExportMembers other{{5, 6, 7, 9}};
        BOOST_CHECK(Contains(refused(fb, cert, *signing, *node::MakeFreshLineageSet(other.snapshot, 4), all, 2, domain),
                             "does not commit"));
    }
    {
        node::FreshLineageSet ruleset1{*signing};
        ruleset1.header.ruleset_version = modern::FINALITY_RULESET_V1;
        BOOST_CHECK(Contains(refused(fb, cert, ruleset1, *successor, all, 2, domain), "ruleset 2"));
    }
    BOOST_CHECK(Contains(refused(fb, set.Sign(modern::FinalityDigestV2(domain, fb), {0, 3}), *signing, *successor,
                                 all, 2, domain),
                         "insufficient-weight"));
    {
        // One heavy signer: the weight quorum without the headcount floor(2n/3)+1.
        const ExportMembers heavy{{20, 1, 1, 1}};
        const auto heavy_signing{node::MakeFreshLineageSet(heavy.snapshot, 3)};
        const auto heavy_successor{node::MakeFreshLineageSet(heavy.snapshot, 4)};
        FinalizedBlockV2 f{fb};
        f.validator_set_hash = heavy_successor->set_hash;
        uint32_t heaviest{0};
        for (uint32_t i{0}; i < heavy.snapshot->Size(); ++i) {
            if (heavy.snapshot->Members()[i].weight == 20) heaviest = i;
        }
        BOOST_CHECK(Contains(refused(f, heavy.Sign(modern::FinalityDigestV2(domain, f), {heaviest}), *heavy_signing,
                                     *heavy_successor, all, 2, domain),
                             "insufficient-headcount"));
    }
    {
        // A V1-tagged signature over the analogous V1 object never verifies under V2.
        const modern::FinalizedBlock v1_fb{fb.height, fb.block_hash, fb.withdrawal_root, fb.validator_set_hash, fb.epoch};
        BOOST_CHECK(Contains(refused(fb, set.Sign(modern::FinalityDigest(domain, v1_fb), {0, 1, 3}), *signing,
                                     *successor, all, 2, domain),
                             "bad-signature"));
    }
    BOOST_CHECK(Contains(refused(fb, cert, *signing, *successor, all, 2, Asym(0xe1)), "bad-signature"));
    BOOST_CHECK(Contains(refused(fb, cert, *signing, *successor, all, 2, uint256{}), "chain domain is zero"));
    {
        FinalizedBlockV2 index0{fb};
        index0.checkpoint_index = 0;
        BOOST_CHECK(Contains(refused(index0, cert, *signing, *successor, {}, 0, domain), "checkpoint index"));
    }
}

BOOST_FIXTURE_TEST_CASE(lineage_rpcs_report_or_refuse_without_the_knob, TestingSetup)
{
    // TestingSetup runs mainnet parameters: no fresh lineage exists there.
    BOOST_REQUIRE(!m_node.chainman->GetConsensus().fresh_lineage.has_value());
    LineageRpc rpc{m_node};
    BOOST_CHECK_EQUAL(rpc.Call("getfinalitylineage").write(), R"({"configured":false})");
    const std::string not_configured{"The fresh finality lineage is not configured on this chain"};
    BOOST_CHECK(Contains(rpc.Error("getfinalitylineagecertificate", {"0"}), not_configured));
    BOOST_CHECK(Contains(rpc.Error("getfinalitylineagecertificate", {"0", "0"}), not_configured));
    BOOST_CHECK(Contains(rpc.Error("getfinalitylineagevotes"), not_configured));
    BOOST_CHECK(Contains(rpc.Error("submitfinalitylineagevote", {std::string(2 * FreshLineageSig::SERIALIZED_SIZE, '0')}),
                         not_configured));
}

BOOST_AUTO_TEST_SUITE_END()
