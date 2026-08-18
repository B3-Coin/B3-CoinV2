// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! Legacy FN issuance (doc/design/b3-legacy-fn-issuance-proposal.md):
//! the proof-carrying builder/verifier model. The merkle fold, the
//! canonical 1-coin-P2PKH recipient rule, the strict proof codec, the
//! pure builder with its self-verification, the stateless verifier with
//! a full sabotage suite, and the archival sweep over a genuinely
//! validated chain. No consensus wiring, no mempool, no wallet — the
//! activation-height rules are a later, separately reviewed step.

#include <arith_uint256.h>
#include <chainparams.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <legacy/codec.h>
#include <legacy/consensus.h>
#include <modern/legacy_fn_issuance.h>
#include <node/blockstorage.h>
#include <node/legacy_fn_issuance.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/time.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <limits>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

using modern::AssetId;
using modern::BuildLegacyFnIssuanceProof;
using modern::DecodeLegacyFnIssuanceProof;
using modern::EncodeLegacyFnIssuanceProof;
using modern::EncodeLegacyTx;
using modern::FindLegacyFnRecipientVout;
using modern::FnAssetId;
using modern::LegacyFnIssuanceFacts;
using modern::LegacyFnIssuanceProofV1;
using modern::LegacyFnRecipientCommitment;
using modern::VerifyLegacyFnIssuanceAction;
using modern::VerifyLegacyFnIssuanceProof;

namespace {

constexpr CAmount COIN_B3{1'000'000};
constexpr CAmount TEST_COLLATERAL{100 * COIN_B3};
constexpr CAmount OFFLINE_TIER{5 * COIN_B3};
constexpr uint32_t GENESIS_TIME{1'400'000'000};
constexpr uint32_t EASY_BITS{0x207fffff};
// Pinned vector: the deterministic legacy-beneficiary ownership
// commitment of P2pkhScript(0x11) — SHA256 of the canonical 25-byte
// P2PKH script (filled from the first computed value, frozen since).
const std::string BENEFICIARY_COMMITMENT_HEX{
    "0e86768a14a61e71306f240f5b8bb92ced2f1abfb246b82d9356549834c2f6e2"};

CScript P2pkhScript(const uint8_t fill)
{
    return CScript() << OP_DUP << OP_HASH160 << std::vector<unsigned char>(20, fill)
                     << OP_EQUALVERIFY << OP_CHECKSIG;
}

CMutableTransaction MakeLegacyTx(const uint32_t ntime)
{
    CMutableTransaction tx;
    tx.version = 1;
    tx.nTime = ntime;
    tx.m_legacy_encoding = true;
    return tx;
}

CMutableTransaction MakeCoinbase(const uint32_t ntime, const int height)
{
    CMutableTransaction tx{MakeLegacyTx(ntime)};
    tx.vin.resize(1);
    tx.vin[0].prevout.SetNull();
    tx.vin[0].scriptSig = CScript() << height << CScriptNum{7};
    tx.vout.emplace_back(0, CScript() << OP_TRUE);
    return tx;
}

//! A structurally boring filler transaction so trees have real depth.
CMutableTransaction MakeDummy(const uint32_t ntime, const uint8_t tag)
{
    CMutableTransaction tx{MakeLegacyTx(ntime)};
    tx.vin.resize(1);
    tx.vin[0].prevout = COutPoint{Txid::FromUint256(uint256{tag}), 0};
    tx.vout.emplace_back(1000 + tag, CScript() << OP_TRUE);
    return tx;
}

CBlock MakeOfflineBlock(std::vector<CMutableTransaction> txs)
{
    CBlock block;
    block.nVersion = 4;
    for (CMutableTransaction& mtx : txs) {
        mtx.m_legacy_encoding = true;
        block.vtx.push_back(MakeTransactionRef(std::move(mtx)));
    }
    block.hashMerkleRoot = BlockMerkleRoot(block);
    return block;
}

//! A fabricated sealed prefix for the pure builder/verifier: five legacy
//! blocks whose committed merkle roots stand in for the block index.
//! Heights are 1-based; H = 5.
struct OfflineChain {
    Consensus::Params params;
    std::vector<CBlock> blocks; // blocks[h - 1] is height h
    // Positions of the interesting transactions.
    int32_t pod_height{4};
    uint32_t pod_position{2};
    uint32_t pod_short_position{3};
    uint32_t pod_nomarker_position{4};
    uint32_t tx64_position{5};
    uint32_t pod_b_position{6};

    OfflineChain()
    {
        params.legacy_b3coin = true;
        params.legacy_fn_collateral_test_override = OFFLINE_TIER;
        params.hard_fork_height = 6; // H = 5
        params.legacy_final_hash = uint256::ONE;
        params.hashGenesisBlock =
            uint256{"2222222222222222222222222222222222222222222222222222222222222222"};

        const uint32_t t{GENESIS_TIME};
        blocks.push_back(MakeOfflineBlock({MakeCoinbase(t + 1, 1)}));

        CMutableTransaction funding_a{MakeLegacyTx(t + 2)};
        funding_a.vin.resize(1);
        funding_a.vin[0].prevout = COutPoint{Txid::FromUint256(uint256{0xaa}), 0};
        funding_a.vout.emplace_back(10 * COIN_B3, CScript() << OP_TRUE);
        funding_a.vout.emplace_back(3 * COIN_B3, CScript() << OP_TRUE);
        blocks.push_back(MakeOfflineBlock({MakeCoinbase(t + 2, 2), funding_a, MakeDummy(t + 2, 0x21)}));

        CMutableTransaction funding_b{MakeLegacyTx(t + 3)};
        funding_b.vin.resize(1);
        funding_b.vin[0].prevout = COutPoint{Txid::FromUint256(uint256{0xbb}), 0};
        funding_b.vout.emplace_back(4 * COIN_B3, CScript() << OP_TRUE);
        CMutableTransaction funding_c{MakeLegacyTx(t + 3)};
        funding_c.vin.resize(1);
        funding_c.vin[0].prevout = COutPoint{Txid::FromUint256(uint256{0xcc}), 0};
        funding_c.vout.emplace_back(6 * COIN_B3, CScript() << OP_TRUE);
        funding_c.vout.emplace_back(7 * COIN_B3, CScript() << OP_TRUE);
        blocks.push_back(MakeOfflineBlock({MakeCoinbase(t + 3, 3), funding_b, funding_c}));

        const Txid funding_a_txid{CTransaction{funding_a}.GetHash()};
        const Txid funding_b_txid{CTransaction{funding_b}.GetHash()};
        const Txid funding_c_txid{CTransaction{funding_c}.GetHash()};

        // The eligible disintegration: 14 B3 in, 8.8 B3 out, gap 5.2 >=
        // tier 5. Output 0 is a DECOY (marker value, not P2PKH); output
        // 1 is the canonical recipient.
        CMutableTransaction pod{MakeLegacyTx(t + 4)};
        pod.vin.resize(2);
        pod.vin[0].prevout = COutPoint{funding_a_txid, 0};
        pod.vin[1].prevout = COutPoint{funding_b_txid, 0};
        pod.vout.emplace_back(COIN_B3, CScript() << OP_TRUE); // decoy: right value, wrong form
        pod.vout.emplace_back(COIN_B3, P2pkhScript(0x11));    // the canonical recipient
        pod.vout.emplace_back(6'800'000, CScript() << OP_TRUE);

        // Qualifying value gap NOT reached: gap 0.5 B3 < tier.
        CMutableTransaction pod_short{MakeLegacyTx(t + 4)};
        pod_short.vin.resize(1);
        pod_short.vin[0].prevout = COutPoint{funding_a_txid, 1};
        pod_short.vout.emplace_back(COIN_B3, P2pkhScript(0x22));
        pod_short.vout.emplace_back(1'500'000, CScript() << OP_TRUE);

        // Qualifying gap but NO 1-coin P2PKH designation: ignored.
        CMutableTransaction pod_nomarker{MakeLegacyTx(t + 4)};
        pod_nomarker.vin.resize(1);
        pod_nomarker.vin[0].prevout = COutPoint{funding_c_txid, 0};
        pod_nomarker.vout.emplace_back(400'000, CScript() << OP_TRUE);

        // Exactly 64 serialized bytes: the merkle leaf/inner-node
        // ambiguity shape the verifier must refuse as evidence.
        CMutableTransaction tx64{MakeLegacyTx(t + 4)};
        tx64.vin.resize(1);
        tx64.vin[0].prevout = COutPoint{Txid::FromUint256(uint256{0x64}), 0};
        tx64.vout.emplace_back(0, CScript{});

        // A SECOND eligible disintegration (7 B3 in, 1.5 B3 out, gap 5.5
        // >= tier) with its own designation — two PoDs, one global asset.
        CMutableTransaction pod_b{MakeLegacyTx(t + 4)};
        pod_b.vin.resize(1);
        pod_b.vin[0].prevout = COutPoint{funding_c_txid, 1};
        pod_b.vout.emplace_back(COIN_B3, P2pkhScript(0x33));
        pod_b.vout.emplace_back(500'000, CScript() << OP_TRUE);

        blocks.push_back(MakeOfflineBlock({MakeCoinbase(t + 4, 4), MakeDummy(t + 4, 0x41), pod,
                                           pod_short, pod_nomarker, tx64, pod_b}));
        blocks.push_back(MakeOfflineBlock({MakeCoinbase(t + 5, 5)}));
    }

    //! The chain's ONE global FN asset id, derived from its synthetic
    //! modern chain domain.
    AssetId FnAsset() const
    {
        const auto domain{modern::ModernChainDomain(params.hashGenesisBlock,
                                                    *params.legacy_final_hash)};
        BOOST_REQUIRE(domain);
        return FnAssetId(*domain);
    }

    modern::LegacyBlockAt BlockAt() const
    {
        return [this](const int height, CBlock& block) {
            if (height < 1 || height > static_cast<int>(blocks.size())) return false;
            block = blocks[height - 1];
            return true;
        };
    }

    //! The fabricated chain view: block "hashes" are stand-ins (the
    //! params' X at H, deterministic fillers elsewhere) — the verifier
    //! only ever compares the hash at H against X.
    modern::LegacyChainView View() const
    {
        return modern::LegacyChainView{
            .block_hash_at = [this](const int height) -> std::optional<uint256> {
                if (height < 1 || height > static_cast<int>(blocks.size())) return std::nullopt;
                if (height == static_cast<int>(blocks.size())) return *params.legacy_final_hash;
                return uint256{static_cast<uint8_t>(height)};
            },
            .merkle_root_at = [this](const int height) -> std::optional<uint256> {
                if (height < 1 || height > static_cast<int>(blocks.size())) return std::nullopt;
                return blocks[height - 1].hashMerkleRoot;
            }};
    }

    modern::LegacyTxLocator Locator() const
    {
        std::map<Txid, std::pair<int32_t, uint32_t>> locations;
        for (size_t h{0}; h < blocks.size(); ++h) {
            for (size_t position{0}; position < blocks[h].vtx.size(); ++position) {
                locations.emplace(blocks[h].vtx[position]->GetHash(),
                                  std::make_pair(static_cast<int32_t>(h + 1),
                                                 static_cast<uint32_t>(position)));
            }
        }
        return [locations](const Txid& txid) -> std::optional<std::pair<int32_t, uint32_t>> {
            const auto it{locations.find(txid)};
            if (it == locations.end()) return std::nullopt;
            return it->second;
        };
    }

    const CTransaction Pod() const { return CTransaction{*blocks[pod_height - 1].vtx[pod_position]}; }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(legacy_fn_issuance_tests, BasicTestingSetup)

//! ComputeMerkleRootFromPath is the exact inverse of
//! TransactionMerklePath for every tree size and position, including the
//! odd-duplication levels.
BOOST_AUTO_TEST_CASE(merkle_fold_roundtrip)
{
    for (int count{1}; count <= 8; ++count) {
        std::vector<CMutableTransaction> txs;
        for (int i{0}; i < count; ++i) {
            txs.push_back(MakeDummy(GENESIS_TIME + i, static_cast<uint8_t>(i + 1)));
        }
        const CBlock block{MakeOfflineBlock(std::move(txs))};
        for (uint32_t position{0}; position < block.vtx.size(); ++position) {
            const std::vector<uint256> path{TransactionMerklePath(block, position)};
            BOOST_CHECK_EQUAL(
                ComputeMerkleRootFromPath(block.vtx[position]->GetHash().ToUint256(), path,
                                          position)
                    .GetHex(),
                block.hashMerkleRoot.GetHex());
        }
        // A wrong position or a tampered path must NOT reach the root.
        if (block.vtx.size() > 1) {
            std::vector<uint256> path{TransactionMerklePath(block, 0)};
            BOOST_CHECK(ComputeMerkleRootFromPath(block.vtx[0]->GetHash().ToUint256(), path, 1) !=
                        block.hashMerkleRoot);
            path[0] = uint256{0xde};
            BOOST_CHECK(ComputeMerkleRootFromPath(block.vtx[0]->GetHash().ToUint256(), path, 0) !=
                        block.hashMerkleRoot);
        }
    }
}

//! The canonical fold closes both known merkle-position malleability
//! sources: unused high bits, and the side bit at an odd-tree
//! self-duplication level (Hash(h, h) is order-independent, so the
//! plain fold accepts BOTH encodings — demonstrated below — and the
//! canonical fold must reject the non-canonical one).
BOOST_AUTO_TEST_CASE(merkle_position_canonicality)
{
    // Five leaves: leaf 4 is self-duplicated at level 0 (and its parent
    // again at level 1), so its path contains its own running hash.
    std::vector<CMutableTransaction> txs;
    for (int i{0}; i < 5; ++i) {
        txs.push_back(MakeDummy(GENESIS_TIME + i, static_cast<uint8_t>(i + 1)));
    }
    const CBlock block{MakeOfflineBlock(std::move(txs))};
    const uint256 leaf{block.vtx[4]->GetHash().ToUint256()};
    const std::vector<uint256> path{TransactionMerklePath(block, 4)};
    uint256 root;
    std::string error;
    BOOST_REQUIRE_MESSAGE(modern::CanonicalMerkleRootFromPath(leaf, path, 4, root, error), error);
    BOOST_CHECK_EQUAL(root.GetHex(), block.hashMerkleRoot.GetHex());
    // Position 5 (flipped bit at the duplicated level) folds to the SAME
    // root through the plain fold — genuine malleability...
    BOOST_CHECK_EQUAL(ComputeMerkleRootFromPath(leaf, path, 5).GetHex(),
                      block.hashMerkleRoot.GetHex());
    // ...which the canonical fold rejects.
    BOOST_CHECK(!modern::CanonicalMerkleRootFromPath(leaf, path, 5, root, error));
    BOOST_CHECK(error.find("not canonical") != std::string::npos);
    // Same story for bits above the path length.
    const uint32_t high{4 | (uint32_t{1} << path.size())};
    BOOST_CHECK_EQUAL(ComputeMerkleRootFromPath(leaf, path, high).GetHex(),
                      block.hashMerkleRoot.GetHex());
    BOOST_CHECK(!modern::CanonicalMerkleRootFromPath(leaf, path, high, root, error));
    BOOST_CHECK(error.find("bits above") != std::string::npos);
}

//! The canonical recipient: lowest-index 1-COIN byte-exact P2PKH output;
//! no such output => the disintegration is ignored.
BOOST_AUTO_TEST_CASE(recipient_rule)
{
    CMutableTransaction tx{MakeLegacyTx(GENESIS_TIME)};
    tx.vin.resize(1);
    tx.vin[0].prevout = COutPoint{Txid::FromUint256(uint256::ONE), 0};

    // No outputs at all.
    BOOST_CHECK(!FindLegacyFnRecipientVout(CTransaction{tx}));
    // Marker value on a non-P2PKH script: not a designation.
    tx.vout.emplace_back(COIN_B3, CScript() << OP_TRUE);
    BOOST_CHECK(!FindLegacyFnRecipientVout(CTransaction{tx}));
    // P2PKH with the wrong value: not a designation.
    tx.vout.emplace_back(COIN_B3 + 1, P2pkhScript(0x33));
    BOOST_CHECK(!FindLegacyFnRecipientVout(CTransaction{tx}));
    // The genuine designation.
    tx.vout.emplace_back(COIN_B3, P2pkhScript(0x44));
    BOOST_REQUIRE(FindLegacyFnRecipientVout(CTransaction{tx}));
    BOOST_CHECK_EQUAL(*FindLegacyFnRecipientVout(CTransaction{tx}), 2U);
    // A second designation later: the lowest index stays canonical.
    tx.vout.emplace_back(COIN_B3, P2pkhScript(0x55));
    BOOST_CHECK_EQUAL(*FindLegacyFnRecipientVout(CTransaction{tx}), 2U);
}

//! Builder end to end on the fabricated sealed prefix, byte-identical
//! rebuilds, the strict codec, and the full sabotage suite.
BOOST_AUTO_TEST_CASE(offline_build_verify_and_sabotage)
{
    const OfflineChain chain;
    const auto block_at{chain.BlockAt()};
    const auto locate{chain.Locator()};
    const auto view{chain.View()};

    // ---- Build + verify the eligible PoD.
    LegacyFnIssuanceProofV1 proof;
    LegacyFnIssuanceFacts facts;
    std::string error;
    BOOST_REQUIRE_MESSAGE(BuildLegacyFnIssuanceProof(chain.pod_height, chain.pod_position,
                                                     chain.params, block_at, locate, view,
                                                     proof, facts, error),
                          error);
    const CTransaction pod{chain.Pod()};
    BOOST_CHECK(facts.pod_id == pod.GetHash());
    BOOST_CHECK_EQUAL(facts.height, chain.pod_height);
    BOOST_CHECK_EQUAL(facts.tier, OFFLINE_TIER);
    BOOST_CHECK_EQUAL(facts.disintegrated, 5'200'000);
    BOOST_CHECK_EQUAL(facts.recipient_vout, 1U); // the decoy at 0 is skipped
    BOOST_CHECK(facts.recipient_script == P2pkhScript(0x11));
    BOOST_REQUIRE_EQUAL(proof.funding.size(), 2U); // distinct txids, first-appearance order

    // ---- Two builds are byte-identical (the builder has no authority).
    {
        LegacyFnIssuanceProofV1 again;
        LegacyFnIssuanceFacts facts_again;
        BOOST_REQUIRE(BuildLegacyFnIssuanceProof(chain.pod_height, chain.pod_position,
                                                 chain.params, block_at, locate, view, again,
                                                 facts_again, error));
        const auto again_bytes{EncodeLegacyFnIssuanceProof(again, error)};
        BOOST_REQUIRE_MESSAGE(again_bytes, error);
        const auto proof_bytes{EncodeLegacyFnIssuanceProof(proof, error)};
        BOOST_REQUIRE_MESSAGE(proof_bytes, error);
        BOOST_CHECK(*again_bytes == *proof_bytes);
    }

    // ---- Strict codec: round trip, then reject every truncation, a
    // trailing byte and an unknown version.
    const std::vector<unsigned char> encoded{[&] {
        const auto bytes{EncodeLegacyFnIssuanceProof(proof, error)};
        BOOST_REQUIRE_MESSAGE(bytes, error);
        return *bytes;
    }()};
    {
        LegacyFnIssuanceProofV1 decoded;
        BOOST_REQUIRE_MESSAGE(DecodeLegacyFnIssuanceProof(encoded, decoded, error), error);
        BOOST_CHECK(decoded == proof);
        for (const size_t len : {size_t{0}, size_t{1}, size_t{5}, encoded.size() / 2,
                                 encoded.size() - 1}) {
            LegacyFnIssuanceProofV1 partial;
            BOOST_CHECK(!DecodeLegacyFnIssuanceProof(
                std::span{encoded.data(), len}, partial, error));
        }
        std::vector<unsigned char> trailing{encoded};
        trailing.push_back(0x00);
        BOOST_CHECK(!DecodeLegacyFnIssuanceProof(trailing, decoded, error));
        BOOST_CHECK(error.find("trailing") != std::string::npos);
        std::vector<unsigned char> versioned{encoded};
        versioned[0] = 2;
        BOOST_CHECK(!DecodeLegacyFnIssuanceProof(versioned, decoded, error));
        BOOST_CHECK(error.find("version") != std::string::npos);
    }

    // ---- Encoder/decoder SYMMETRY: the encoder refuses every
    // structurally malformed object its decoder would reject — it can
    // never emit bytes that fail to decode.
    {
        const auto encode_rejects{[&](const LegacyFnIssuanceProofV1& bad, const char* needle) {
            std::string enc_error;
            const auto bytes{EncodeLegacyFnIssuanceProof(bad, enc_error)};
            BOOST_CHECK_MESSAGE(!bytes, "encoder unexpectedly accepted (wanted: " << needle
                                                                                 << ")");
            BOOST_CHECK_MESSAGE(enc_error.find(needle) != std::string::npos,
                                "error '" << enc_error << "' does not mention '" << needle
                                          << "'");
            // The same object also fails the action-level encoder.
            BOOST_CHECK(!modern::EncodeLegacyFnIssuanceAction(
                modern::LegacyFnIssuanceActionV1{.fn_output_index = 0, .proof = bad}));
        }};
        LegacyFnIssuanceProofV1 bad{proof};
        bad.pod.height = 0;
        encode_rejects(bad, "height out of range");
        bad = proof;
        bad.pod.height = -3;
        encode_rejects(bad, "height out of range");
        bad = proof;
        bad.funding[0].tx_bytes.clear();
        encode_rejects(bad, "transaction length out of range");
        bad = proof;
        bad.pod.tx_bytes.assign(modern::MAX_LEGACY_FN_EVIDENCE_TX_SIZE + 1, 0x00);
        encode_rejects(bad, "transaction length out of range");
        bad = proof;
        bad.funding[0].path.assign(modern::MAX_LEGACY_FN_MERKLE_PATH + 1, uint256::ONE);
        encode_rejects(bad, "path length out of range");
        // And a valid proof always round-trips: encode → decode → equal.
        std::string sym_error;
        const auto bytes{EncodeLegacyFnIssuanceProof(proof, sym_error)};
        BOOST_REQUIRE_MESSAGE(bytes, sym_error);
        LegacyFnIssuanceProofV1 decoded;
        BOOST_REQUIRE_MESSAGE(DecodeLegacyFnIssuanceProof(*bytes, decoded, sym_error),
                              sym_error);
        BOOST_CHECK(decoded == proof);
    }

    // ---- The complete inactive issuance (corrected model, owner ruling
    // 2026-08-18): the action authorizes +1 unit of the ONE global FN
    // asset at one output bound to the historical beneficiary. Accept,
    // then reject every binding fault of §22.
    const auto never_issued{[](const Txid&) { return false; }};
    const AssetId fn_asset{chain.FnAsset()};
    const uint256 beneficiary{LegacyFnRecipientCommitment(facts.recipient_script)};
    // The deterministic beneficiary → ownership-commitment mapping,
    // pinned: SHA256 of the canonical 25-byte P2PKH script (the
    // script-hash ownership form; no pubkey is known or needed).
    BOOST_CHECK_EQUAL(LegacyFnRecipientCommitment(P2pkhScript(0x11)).GetHex(),
                      BENEFICIARY_COMMITMENT_HEX);
    const auto issuance_outputs{[&](const CAmount units, const uint256& commitment,
                                    const AssetId& asset) {
        const auto out{modern::MakeFnOutput(
            modern::FnOutputView{.amount = units, .owner_commitment = commitment}, asset)};
        BOOST_REQUIRE(out);
        return std::vector<modern::ModernOutput>{*out};
    }};
    const modern::LegacyFnIssuanceActionV1 action{.fn_output_index = 0, .proof = proof};
    {
        modern::LegacyFnMintAuthorization mint;
        const auto outputs{issuance_outputs(1, beneficiary, fn_asset)};
        BOOST_CHECK_MESSAGE(VerifyLegacyFnIssuanceAction(action, outputs,
                                                         chain.params, view, never_issued,
                                                         mint, error),
                            error);
        BOOST_CHECK(mint.pod_id == facts.pod_id);
        BOOST_CHECK_EQUAL(mint.fn_output_index, 0U);
        BOOST_CHECK_EQUAL(mint.amount, 1);
        BOOST_CHECK(mint.recipient_policy_commitment == beneficiary);
        // The authorized output carries no PoDId and mints no native B3.
        BOOST_CHECK(outputs[0].policy_params.empty());
        BOOST_CHECK(outputs[0].asset != modern::NativeAsset());

        // Action codec: CreationAction round trip (type 2), and the
        // RESERVED superseded FnClaimActionV1 type is never accepted.
        const auto encoded{modern::EncodeLegacyFnIssuanceAction(action)};
        BOOST_REQUIRE(encoded);
        BOOST_CHECK_EQUAL(encoded->action_type, modern::CREATION_ACTION_LEGACY_FN_ISSUANCE);
        modern::LegacyFnIssuanceActionV1 decoded;
        BOOST_REQUIRE_MESSAGE(modern::DecodeLegacyFnIssuanceAction(*encoded, decoded, error),
                              error);
        BOOST_CHECK(decoded == action);
        modern::CreationAction superseded{*encoded};
        superseded.action_type = modern::CREATION_ACTION_FN_CLAIM;
        BOOST_CHECK(!modern::DecodeLegacyFnIssuanceAction(superseded, decoded, error));
        BOOST_CHECK(error.find("superseded or unknown") != std::string::npos);
        modern::CreationAction truncated{*encoded};
        truncated.payload.pop_back();
        BOOST_CHECK(!modern::DecodeLegacyFnIssuanceAction(truncated, decoded, error));

        // Two distinct PoDs authorize units of the SAME global asset:
        // pod_b's issuance mints FN_ASSET_ID too, with its own PoDId.
        LegacyFnIssuanceProofV1 proof_b;
        LegacyFnIssuanceFacts facts_b;
        BOOST_REQUIRE_MESSAGE(BuildLegacyFnIssuanceProof(chain.pod_height,
                                                         chain.pod_b_position, chain.params,
                                                         block_at, locate, view, proof_b,
                                                         facts_b, error),
                              error);
        BOOST_CHECK(facts_b.pod_id != facts.pod_id); // PoDIdA != PoDIdB
        const uint256 beneficiary_b{LegacyFnRecipientCommitment(facts_b.recipient_script)};
        modern::LegacyFnMintAuthorization mint_b;
        const auto outputs_b{issuance_outputs(1, beneficiary_b, fn_asset)};
        BOOST_CHECK_MESSAGE(
            VerifyLegacyFnIssuanceAction({.fn_output_index = 0, .proof = proof_b}, outputs_b,
                                         chain.params, view, never_issued, mint_b,
                                         error),
            error);
        // FNAssetA == FNAssetB == FN_ASSET_ID; +1 unit each.
        BOOST_CHECK(outputs[0].asset == outputs_b[0].asset);
        BOOST_CHECK(outputs_b[0].asset == fn_asset);
        BOOST_CHECK_EQUAL(mint_b.amount, 1);
    }
    // Every §22 binding fault rejects. The verifier derives the FN asset
    // INTERNALLY from consensus params — no caller nominates it.
    {
        modern::LegacyFnMintAuthorization mint;
        const auto reject{[&](const std::vector<modern::ModernOutput>& outputs,
                              const modern::LegacyFnIssuanceActionV1& bad_action,
                              const Consensus::Params& bad_params, const char* needle) {
            std::string bad_error;
            BOOST_CHECK_MESSAGE(!VerifyLegacyFnIssuanceAction(bad_action, outputs,
                                                              bad_params, view, never_issued,
                                                              mint, bad_error),
                                "unexpectedly accepted (wanted: " << needle << ")");
            BOOST_CHECK_MESSAGE(bad_error.find(needle) != std::string::npos,
                                "error '" << bad_error << "' does not mention '" << needle
                                          << "'");
        }};
        // Native asset as the FN output's asset (raw mutation — the
        // builder refuses to construct such an output).
        {
            auto outputs{issuance_outputs(1, beneficiary, fn_asset)};
            outputs[0].asset = modern::NativeAsset();
            reject(outputs, action, chain.params, "must not carry the native asset");
        }
        // Some other, non-FN asset id (raw mutation).
        {
            auto outputs{issuance_outputs(1, beneficiary, fn_asset)};
            outputs[0].asset = uint256::ONE;
            reject(outputs, action, chain.params, "not the chain's FN asset id");
        }
        // A caller cannot BLESS an arbitrary asset: an output legally
        // built for some other (non-native) asset id still rejects,
        // because the verifier derives the chain's FN asset itself.
        {
            const AssetId arbitrary{uint256{"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"}};
            reject(issuance_outputs(1, beneficiary, arbitrary), action, chain.params,
                   "not the chain's FN asset id");
        }
        // Without a derivable modern chain domain there is NO FN asset:
        // fail closed.
        {
            Consensus::Params no_domain{chain.params};
            no_domain.hashGenesisBlock = uint256{};
            reject(issuance_outputs(1, beneficiary, fn_asset), action, no_domain,
                   "cannot be derived");
        }
        // Exactly one unit per issuance. Amount 0 must be a RAW mutation:
        // a zero-amount FN output cannot even be built (zero balance is
        // represented by no output), so the parse layer rejects it.
        {
            auto outputs{issuance_outputs(1, beneficiary, fn_asset)};
            outputs[0].amount = 0;
            reject(outputs, action, chain.params, "malformed FN output");
        }
        reject(issuance_outputs(2, beneficiary, fn_asset), action, chain.params,
               "exactly one FN unit");
        // Wrong policy type / version / non-canonical params.
        {
            auto outputs{issuance_outputs(1, beneficiary, fn_asset)};
            outputs[0].policy_type = static_cast<uint16_t>(modern::PolicyType::OWNER);
            reject(outputs, action, chain.params, "malformed FN output");
        }
        {
            auto outputs{issuance_outputs(1, beneficiary, fn_asset)};
            outputs[0].policy_version = 2;
            reject(outputs, action, chain.params, "malformed FN output");
        }
        {
            auto outputs{issuance_outputs(1, beneficiary, fn_asset)};
            outputs[0].policy_params.push_back(0x00);
            reject(outputs, action, chain.params, "params must be empty");
        }
        // Wrong / out-of-range output index.
        {
            modern::LegacyFnIssuanceActionV1 bad{action};
            bad.fn_output_index = 1;
            reject(issuance_outputs(1, beneficiary, fn_asset), bad, chain.params,
                   "nonexistent output");
        }
        // Wrong beneficiary commitment.
        reject(issuance_outputs(1, LegacyFnRecipientCommitment(P2pkhScript(0x99)), fn_asset),
               action, chain.params, "historical beneficiary");
        // Duplicate issuance: the injected predicate says already issued.
        {
            std::string dup_error;
            BOOST_CHECK(!VerifyLegacyFnIssuanceAction(
                action, issuance_outputs(1, beneficiary, fn_asset), chain.params,
                view, [](const Txid&) { return true; }, mint, dup_error));
            BOOST_CHECK(dup_error.find("already issued") != std::string::npos);
        }
    }

    // ---- Sabotage the proof, one field at a time.
    const auto rejected{[&](const LegacyFnIssuanceProofV1& bad, const char* needle) {
        LegacyFnIssuanceFacts out;
        std::string bad_error;
        const bool ok{VerifyLegacyFnIssuanceProof(bad, chain.params, view, out, bad_error)};
        BOOST_CHECK_MESSAGE(!ok, "sabotaged proof unexpectedly accepted (wanted: " << needle << ")");
        BOOST_CHECK_MESSAGE(bad_error.find(needle) != std::string::npos,
                            "error '" << bad_error << "' does not mention '" << needle << "'");
    }};
    {
        LegacyFnIssuanceProofV1 bad{proof}; // tampered pod merkle path
        bad.pod.path[0] = uint256{0xde};
        rejected(bad, "merkle path");
    }
    {
        LegacyFnIssuanceProofV1 bad{proof}; // wrong pod position
        bad.pod.position += 1;
        rejected(bad, "merkle path");
    }
    {
        LegacyFnIssuanceProofV1 bad{proof}; // wrong (but existing) height
        bad.pod.height = 3;
        rejected(bad, "merkle path");
    }
    {
        LegacyFnIssuanceProofV1 bad{proof}; // height above the sealed prefix
        bad.pod.height = 6;
        rejected(bad, "sealed legacy prefix");
    }
    {
        LegacyFnIssuanceProofV1 bad{proof}; // tampered pod bytes (txid changes)
        bad.pod.tx_bytes.back() ^= 0x01;
        rejected(bad, "merkle path");
    }
    {
        LegacyFnIssuanceProofV1 bad{proof}; // tampered funding bytes
        bad.funding[0].tx_bytes.back() ^= 0x01;
        rejected(bad, "funding evidence");
    }
    {
        LegacyFnIssuanceProofV1 bad{proof}; // dropped funding entry
        bad.funding.pop_back();
        rejected(bad, "count");
    }
    {
        LegacyFnIssuanceProofV1 bad{proof}; // duplicated entry (count right, txid wrong)
        bad.funding[1] = bad.funding[0];
        rejected(bad, "does not match");
    }
    {
        LegacyFnIssuanceProofV1 bad{proof}; // reordered entries
        std::swap(bad.funding[0], bad.funding[1]);
        rejected(bad, "does not match");
    }
    {
        // A REAL chain transaction substituted for a funding entry:
        // valid membership, wrong identity.
        LegacyFnIssuanceProofV1 bad{proof};
        const CBlock& block2{chain.blocks[1]};
        bad.funding[0].tx_bytes = EncodeLegacyTx(*block2.vtx[2]); // the dummy
        bad.funding[0].height = 2;
        bad.funding[0].position = 2;
        bad.funding[0].path = TransactionMerklePath(block2, 2);
        rejected(bad, "does not match");
    }
    {
        LegacyFnIssuanceProofV1 bad{proof}; // trailing byte inside evidence
        bad.pod.tx_bytes.push_back(0x00);
        rejected(bad, "trailing");
    }

    // ---- Merkle-position malleability is closed: bits above the path
    // length fold to the same root (they are never consumed), so before
    // the canonicality rule this byte-different proof VERIFIED.
    {
        LegacyFnIssuanceProofV1 bad{proof};
        bad.pod.position |= (uint32_t{1} << bad.pod.path.size());
        rejected(bad, "bits above the path length");
    }

    // ---- Unpinned parameters: nothing is sealed, nothing verifies.
    {
        Consensus::Params unpinned{chain.params};
        unpinned.hard_fork_height = std::nullopt;
        unpinned.legacy_final_hash = std::nullopt;
        LegacyFnIssuanceFacts out;
        BOOST_CHECK(!VerifyLegacyFnIssuanceProof(proof, unpinned, view, out, error));
        BOOST_CHECK(error.find("not pinned") != std::string::npos);
    }

    // ---- X-anchor: the verifier trusts NO merkle roots from a view
    // whose block at H is not exactly X (or that cannot answer at all);
    // the genuine view — X exactly at H — accepts.
    {
        LegacyFnIssuanceFacts out;
        modern::LegacyChainView wrong_x{view};
        wrong_x.block_hash_at = [](const int) -> std::optional<uint256> { return uint256{0x99}; };
        BOOST_CHECK(!VerifyLegacyFnIssuanceProof(proof, chain.params, wrong_x, out, error));
        BOOST_CHECK(error.find("does not carry X") != std::string::npos);
        modern::LegacyChainView no_hash{view};
        no_hash.block_hash_at = nullptr;
        BOOST_CHECK(!VerifyLegacyFnIssuanceProof(proof, chain.params, no_hash, out, error));
        BOOST_CHECK(error.find("does not carry X") != std::string::npos);
        BOOST_CHECK_MESSAGE(VerifyLegacyFnIssuanceProof(proof, chain.params, view, out, error),
                            error);
    }

    // ---- The builder refuses ineligible disintegrations outright.
    {
        LegacyFnIssuanceProofV1 unused;
        LegacyFnIssuanceFacts out;
        BOOST_CHECK(!BuildLegacyFnIssuanceProof(chain.pod_height, chain.pod_short_position,
                                                chain.params, block_at, locate, view, unused,
                                                out, error));
        BOOST_CHECK(error.find("below the collateral tier") != std::string::npos);
        BOOST_CHECK(!BuildLegacyFnIssuanceProof(chain.pod_height, chain.pod_nomarker_position,
                                                chain.params, block_at, locate, view, unused,
                                                out, error));
        BOOST_CHECK(error.find("ignored") != std::string::npos);
        // The 64-byte shape cannot serve as evidence at all.
        BOOST_REQUIRE_EQUAL(
            EncodeLegacyTx(*chain.blocks[chain.pod_height - 1].vtx[chain.tx64_position]).size(),
            64U);
        BOOST_CHECK(!BuildLegacyFnIssuanceProof(chain.pod_height, chain.tx64_position,
                                                chain.params, block_at, locate, view, unused,
                                                out, error));
        BOOST_CHECK(error.find("64-byte") != std::string::npos);
    }
}

namespace {

//! Disk-backed legacy-B3 regtest for the archival sweep (mirrors the
//! fn_pod fixture: authentic PoW blocks through ProcessNewBlock).
CBlock MakeIssuanceGenesis()
{
    CMutableTransaction coinbase;
    coinbase.version = 1;
    coinbase.nTime = GENESIS_TIME;
    coinbase.m_legacy_encoding = true;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = CScript() << CScriptNum{0} << CScriptNum{7};
    coinbase.vout.emplace_back(0, CScript{});
    CBlock genesis;
    genesis.nVersion = 1;
    genesis.hashPrevBlock.SetNull();
    genesis.nTime = GENESIS_TIME;
    genesis.nBits = EASY_BITS;
    genesis.nNonce = 0;
    genesis.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
    genesis.hashMerkleRoot = BlockMerkleRoot(genesis);
    return genesis;
}

struct IssuanceChainSetup : public ChainTestingSetup {
    IssuanceChainSetup()
        : ChainTestingSetup{ChainType::REGTEST,
                            {.extra_args = {"-acceptnonstdtxn=1"},
                             .coins_db_in_memory = false,
                             .block_tree_db_in_memory = false}}
    {
        SetMockTime(GENESIS_TIME + 1000);
        auto& consensus{const_cast<Consensus::Params&>(m_node.chainman->GetConsensus())};
        consensus.legacy_b3coin = true;
        consensus.legacy_last_pow_block = 1000;
        consensus.legacy_fn_collateral_test_override = TEST_COLLATERAL;
        consensus.BIP34Height = std::numeric_limits<int>::max();
        consensus.BIP65Height = std::numeric_limits<int>::max();
        consensus.BIP66Height = std::numeric_limits<int>::max();
        consensus.CSVHeight = std::numeric_limits<int>::max();
        consensus.SegwitHeight = std::numeric_limits<int>::max();
        CBlock& genesis{const_cast<CBlock&>(m_node.chainman->GetParams().GenesisBlock())};
        genesis = MakeIssuanceGenesis();
        consensus.hashGenesisBlock = genesis.GetLegacyB3Hash();
        LoadVerifyActivateChainstate();
    }
    ~IssuanceChainSetup()
    {
        // The fixture pins H/X on the GLOBAL chainparams object; undo it
        // so later tests see stock regtest.
        auto& consensus{const_cast<Consensus::Params&>(m_node.chainman->GetConsensus())};
        consensus.hard_fork_height = std::nullopt;
        consensus.legacy_final_hash = std::nullopt;
    }
};

} // namespace

//! The archival sweep on a genuinely validated chain: discovery through
//! the undo-based detector, the eligibility filter, funding location,
//! canonical proof construction, and independent stateless verification
//! against the node's own committed merkle roots.
BOOST_FIXTURE_TEST_CASE(archival_sweep_end_to_end, IssuanceChainSetup)
{
    ChainstateManager& chainman{*m_node.chainman};
    const Consensus::Params& consensus{chainman.GetConsensus()};
    auto& mutable_consensus{const_cast<Consensus::Params&>(consensus)};
    const auto tip{[&] { return WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()); }};

    const auto submit{[&](std::vector<CMutableTransaction> txs) {
        const CBlockIndex* prev{tip()};
        CMutableTransaction coinbase;
        coinbase.version = 1;
        coinbase.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 360);
        coinbase.m_legacy_encoding = true;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << (prev->nHeight + 1) << CScriptNum{9};
        coinbase.vout.emplace_back(legacy::GetProofOfWorkReward(0, prev->nHeight + 1, consensus),
                                   CScript() << OP_TRUE);
        CBlock block;
        block.nVersion = 4;
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = coinbase.nTime;
        block.nBits = legacy::GetNextTargetRequired(prev, /*proof_of_stake=*/false, consensus);
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        for (CMutableTransaction& mtx : txs) {
            mtx.m_legacy_encoding = true;
            block.vtx.push_back(MakeTransactionRef(std::move(mtx)));
        }
        block.hashMerkleRoot = BlockMerkleRoot(block);
        const arith_uint256 target{arith_uint256().SetCompact(block.nBits)};
        block.nNonce = 0;
        while (UintToArith256(block.GetLegacyB3Hash()) > target) ++block.nNonce;
        SetMockTime(static_cast<int64_t>(block.nTime) + 600);
        DataStream bytes;
        bytes << legacy::TX_LEGACY(block);
        auto decoded{std::make_shared<CBlock>()};
        bytes >> legacy::TX_LEGACY(*decoded);
        bool new_block{false};
        BOOST_REQUIRE(chainman.ProcessNewBlock(decoded, true, true, &new_block));
        BOOST_REQUIRE_EQUAL(tip()->nHeight, prev->nHeight + 1);
    }};

    // 32 plain blocks; a fan at 33 creating two OP_TRUE funding outputs;
    // the eligible PoD at 34 (with the 1-coin P2PKH designation); an
    // IGNORED qualifying PoD at 35 (no designation); a plain block at 36.
    for (int height{1}; height <= 32; ++height) submit({});
    const Txid coinbase1{[&] {
        CBlock block;
        BOOST_REQUIRE(chainman.m_blockman.ReadBlock(
            block, *WITH_LOCK(cs_main, return chainman.ActiveChain()[1])));
        return block.vtx[0]->GetHash();
    }()};
    const CAmount reward1{legacy::GetProofOfWorkReward(0, 1, consensus)};
    CMutableTransaction fan;
    fan.version = 1;
    fan.nTime = static_cast<uint32_t>(tip()->GetBlockTime() + 360);
    fan.vin.resize(1);
    fan.vin[0].prevout = COutPoint{coinbase1, 0};
    fan.vin[0].scriptSig = CScript{};
    fan.vout.emplace_back(150 * COIN_B3, CScript() << OP_TRUE);
    fan.vout.emplace_back(140 * COIN_B3, CScript() << OP_TRUE);
    fan.vout.emplace_back(reward1 - 290 * COIN_B3 - 1000, CScript() << OP_TRUE);
    submit({fan}); // 33
    const Txid fan_txid{[&] {
        CBlock block;
        BOOST_REQUIRE(chainman.m_blockman.ReadBlock(
            block, *WITH_LOCK(cs_main, return chainman.ActiveChain()[33])));
        return block.vtx[1]->GetHash();
    }()};
    const CScript recipient{P2pkhScript(0x77)};
    CMutableTransaction pod;
    pod.version = 1;
    pod.nTime = static_cast<uint32_t>(tip()->GetBlockTime() + 360);
    pod.vin.resize(1);
    pod.vin[0].prevout = COutPoint{fan_txid, 0};
    pod.vin[0].scriptSig = CScript{};
    pod.vout.emplace_back(COIN_B3, recipient); // the designation
    pod.vout.emplace_back(150 * COIN_B3 - TEST_COLLATERAL - COIN_B3 - 1000,
                          CScript() << OP_TRUE);
    submit({pod}); // 34
    CMutableTransaction pod_ignored;
    pod_ignored.version = 1;
    pod_ignored.nTime = static_cast<uint32_t>(tip()->GetBlockTime() + 360);
    pod_ignored.vin.resize(1);
    pod_ignored.vin[0].prevout = COutPoint{fan_txid, 1};
    pod_ignored.vin[0].scriptSig = CScript{};
    pod_ignored.vout.emplace_back(140 * COIN_B3 - TEST_COLLATERAL - 1000, CScript() << OP_TRUE);
    submit({pod_ignored}); // 35
    submit({});            // 36

    // Unpinned: the sweep refuses — there is no sealed prefix yet.
    std::vector<node::LegacyFnIssuanceCandidate> candidates;
    std::string error;
    BOOST_CHECK(!node::BuildAllLegacyFnIssuances(chainman, candidates, error));
    BOOST_CHECK(error.find("not pinned") != std::string::npos);

    // Pin H = 36, X = the genuine block.
    mutable_consensus.hard_fork_height = 37;
    mutable_consensus.legacy_final_hash = tip()->GetBlockHash();

    BOOST_REQUIRE_MESSAGE(node::BuildAllLegacyFnIssuances(chainman, candidates, error), error);
    BOOST_REQUIRE_EQUAL(candidates.size(), 1U); // the ignored PoD stayed ignored
    const node::LegacyFnIssuanceCandidate& item{candidates[0]};
    BOOST_CHECK_EQUAL(item.facts.height, 34);
    BOOST_CHECK_EQUAL(item.facts.tier, TEST_COLLATERAL);
    BOOST_CHECK_EQUAL(item.facts.disintegrated, TEST_COLLATERAL + 1000);
    BOOST_CHECK_EQUAL(item.facts.recipient_vout, 0U);
    BOOST_CHECK(item.facts.recipient_script == recipient);

    // Independent stateless verification against the node's own
    // committed merkle roots (the block index, exactly what any modern
    // node holds), plus the full binding and the double-issue rejection.
    const modern::LegacyChainView node_view{
        .block_hash_at = [&](const int height) -> std::optional<uint256> {
            LOCK(cs_main);
            if (height <= 0 || height > 36) return std::nullopt;
            const CBlockIndex* pindex{chainman.ActiveChain()[height]};
            if (!pindex) return std::nullopt;
            return pindex->GetBlockHash();
        },
        .merkle_root_at = [&](const int height) -> std::optional<uint256> {
            LOCK(cs_main);
            if (height <= 0 || height > 36) return std::nullopt;
            const CBlockIndex* pindex{chainman.ActiveChain()[height]};
            if (!pindex) return std::nullopt;
            return pindex->hashMerkleRoot;
        }};
    {
        LegacyFnIssuanceFacts out;
        BOOST_CHECK_MESSAGE(
            VerifyLegacyFnIssuanceProof(item.proof, consensus, node_view, out, error), error);
        BOOST_CHECK(out.pod_id == item.facts.pod_id);
        // The complete inactive issuance on the real chain: +1 unit of
        // the chain's global FN asset to the historical beneficiary.
        const auto domain{modern::ModernChainDomain(consensus.hashGenesisBlock,
                                                    *consensus.legacy_final_hash)};
        BOOST_REQUIRE(domain);
        const AssetId fn_asset{FnAssetId(*domain)};
        BOOST_CHECK(fn_asset != modern::NativeAsset());
        const auto fn_out{modern::MakeFnOutput(
            modern::FnOutputView{
                .amount = 1,
                .owner_commitment = LegacyFnRecipientCommitment(out.recipient_script)},
            fn_asset)};
        BOOST_REQUIRE(fn_out);
        const std::vector<modern::ModernOutput> outputs{*fn_out};
        const modern::LegacyFnIssuanceActionV1 action{.fn_output_index = 0,
                                                      .proof = item.proof};
        modern::LegacyFnMintAuthorization mint;
        BOOST_CHECK_MESSAGE(VerifyLegacyFnIssuanceAction(action, outputs, consensus,
                                                         node_view,
                                                         [](const Txid&) { return false; },
                                                         mint, error),
                            error);
        BOOST_CHECK(mint.pod_id == item.facts.pod_id);
        BOOST_CHECK(!VerifyLegacyFnIssuanceAction(action, outputs, consensus,
                                                  node_view, [](const Txid&) { return true; },
                                                  mint, error));
    }

    // The sweep is deterministic: a second run yields identical bytes.
    {
        std::vector<node::LegacyFnIssuanceCandidate> again;
        BOOST_REQUIRE_MESSAGE(node::BuildAllLegacyFnIssuances(chainman, again, error), error);
        BOOST_REQUIRE_EQUAL(again.size(), 1U);
        const auto again_bytes{EncodeLegacyFnIssuanceProof(again[0].proof, error)};
        BOOST_REQUIRE_MESSAGE(again_bytes, error);
        const auto item_bytes{EncodeLegacyFnIssuanceProof(item.proof, error)};
        BOOST_REQUIRE_MESSAGE(item_bytes, error);
        BOOST_CHECK(*again_bytes == *item_bytes);
    }
}

BOOST_AUTO_TEST_SUITE_END()
