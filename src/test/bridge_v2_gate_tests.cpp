// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Bridge V2 node wiring on a real modern-PoS chain (stage S3): the V2 burn
// gate (node changes section 8.4, rows C1-C4 and E1-E8), kind 9 Ethereum
// state observations with their VERIFIER_E verdicts, and the V2 receipt
// limits, as mempool admission, block validation, block assembly and wallet
// bridge prevalidation apply them. The pure predicates are covered by
// bridge_burn_gate_v2_tests, bridge_state_observation_tests and
// bridge_state_tests; this file shows that the four paths agree.
//
// FinalityChainFixture's chain (legacy prefix, corridor, four staked and
// bound validators A, B, C, D with weights 15, 1, 10, 8) gains a managed V1
// bUSD bridge from M + 1 whose V1 withdrawal height is also M + 1 (so the V1
// readiness alone would say READY for every burn), the fresh lineage from
// R_f = M + 11 (genesis G = M + 10) and a regtest-shaped bridge V2 envelope
// from V = W2 = R_f + 1 with gate profile R. Fresh certificates signed by
// the four validators finalize the fresh grid as blocks are produced.
//
// Ethereum is synthetic. Keccak-linked execution headers 100-108 carry the
// state roots of a synthetic Ethereum state (test/util/bridge_observation_
// world.h) holding Verifier V2, Vault V2 and the token, and block 101 also
// carries a Vault V2 deposit receipt. A regtest synthetic beacon
// (bridge/regtest_beacon.h: DefaultGenesisValidatorsRoot, fork 0xb3000001)
// lies over them, so kind 1 and kind 2 records drive the unchanged V2 light
// client and connect real anchors with those state roots. The Verifier V2 in
// that state accepted B3's own canonical statement at the first fresh
// checkpoint (ExpectedFinalizedBlockV2, its certId and the lineage's set
// hashes), so B3 judges its VERIFIER_E verdict 0. A real kind 3 mint of the
// deposit funds the reserve ledger.
//
// The kind 6 burns are synthetic: they spend a native funding output and
// burn bUSD V2 they do not hold, so they can never be valid. Every gate and
// reserve check runs before asset conservation, so a READY gate shows as a
// bad-asset-transition refusal (in block assembly, as a template that fails
// TestBlockValidity because the assembler selected the burn), while a closed
// gate shows as bridge-v2-burn-not-ready (and a burn the assembler skips).
// A kind 9 record earlier in the same block that closes the gate shows as
// bad-bridge-state at the burn (the per-burn gate, node changes section 7.2
// check 4), and block assembly then carries the record and skips the burn.
// The block-assembly cases put a transaction straight into the mempool, as
// if it had been admitted earlier.
//
// sponsored_sighash_in_mempool_and_blocks pays the deposit to a key the test
// holds and burns real bUSD V2: owner ruling R10's sponsored sighash rule
// (SIGHASH_ALL|ANYONECANPAY on a kind 6 burn, from the envelope's own pin)
// through mempool admission and block validation.

#include <addresstype.h>
#include <bridge/admission.h>
#include <bridge/burn_gate_v2.h>
#include <bridge/deposit.h>
#include <bridge/eth_light_client.h>
#include <bridge/proof.h>
#include <bridge/regtest_beacon.h>
#include <bridge/rlp.h>
#include <bridge/ssz.h>
#include <bridge/state_observation.h>
#include <common/args.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <crypto/bls.h>
#include <crypto/common.h>
#include <crypto/keccak256.h>
#include <interfaces/chain.h>
#include <key.h>
#include <modern/asset_output.h>
#include <modern/bridge_binding.h>
#include <modern/bridge_v2.h>
#include <modern/finality_schedule.h>
#include <modern/finality_types.h>
#include <modern/finality_v2.h>
#include <modern/mpa.h>
#include <modern/payload_root.h>
#include <modern/pos_v1.h>
#include <node/bridge_state.h>
#include <node/finality_fresh_lineage.h>
#include <node/finality_tracker.h>
#include <node/miner.h>
#include <primitives/transaction.h>
#include <rpc/client.h>
#include <rpc/register.h>
#include <rpc/request.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <script/interpreter.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <test/util/bridge_observation_world.h>
#include <test/util/finality_fixture.h>
#include <test/util/setup_common.h>
#include <test/util/txmempool.h>
#include <txmempool.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/time.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using b3test::FinalityChainFixture;

constexpr uint8_t SUBJECT_VERIFIER{bridge::ETH_OBSERVATION_SUBJECT_VERIFIER};
constexpr uint8_t SUBJECT_RESERVE{bridge::ETH_OBSERVATION_SUBJECT_RESERVE};
constexpr uint8_t SUBJECT_TOKEN{bridge::ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS};

//! Native fee of every chained bridge carrier.
constexpr CAmount CARRIER_FEE{200'000};
//! The first synthetic execution block, and the beacon period it opens.
constexpr uint64_t EXEC_FIRST{100};
constexpr uint64_t BEACON_PERIOD{3};
//! The one Vault V2 deposit the chain mints.
constexpr uint64_t DEPOSIT_ID{0};
constexpr CAmount DEPOSIT_AMOUNT{60};
//! The synthetic burn's amount.
constexpr CAmount BURN_AMOUNT{5};

const std::string WRONG_VERDICT{"bridge observation verdict does not match B3's lineage"};

Consensus::BridgeEthAddress FillAddress(const uint8_t fill)
{
    Consensus::BridgeEthAddress out;
    out.fill(fill);
    return out;
}

uint256 Keccak(std::span<const unsigned char> bytes)
{
    uint256 hash;
    Keccak256().Write(bytes).Finalize(hash);
    return hash;
}

std::vector<unsigned char> RlpBytes(std::span<const unsigned char> bytes)
{
    return bridge::RlpEncodeBytes(bytes);
}

std::vector<unsigned char> RlpHash(const uint256& hash)
{
    return RlpBytes(std::span<const unsigned char>{hash.begin(), 32});
}

//! A managed V1 bUSD bridge active from `activation` (bridge_state_tests'
//! BridgeParams() pins).
Consensus::BridgeAssetParams ManagedBusd(const int activation)
{
    Consensus::BridgeAssetParams busd;
    busd.asset = Consensus::ETHEREUM_MAINNET_BUSD_IDENTITY;
    busd.vault_runtime_code_hash = uint256{uint8_t{6}};
    busd.implementation_or_adapter = uint256{uint8_t{3}};
    busd.adapter_version = 1;
    busd.origin_deployment_block = 1;
    busd.recipient_encoding_version = Consensus::BRIDGE_RECIPIENT_VERSION_P2PKH_V1;
    busd.activation_height = activation;
    busd.mint_caps = Consensus::BridgeMintCaps{
        .max_per_block = 100,
        .max_per_epoch = 150,
        .epoch_length_blocks = 10,
    };
    Consensus::EthereumLightClientPins light;
    light.trusted_checkpoint_root = uint256{uint8_t{4}};
    light.trusted_checkpoint_slot = 32;
    light.genesis_validators_root = Consensus::ETHEREUM_MAINNET_GENESIS_VALIDATORS_ROOT;
    light.fork_schedule = {{0, {0, 0, 0, 0}}};
    light.fork_schedule_valid_through_epoch = 100;
    light.min_sync_committee_participants = Consensus::ETHEREUM_SYNC_COMMITTEE_SUPERMAJORITY;
    light.max_sync_lag_slots = 10;
    busd.light_client = light;
    busd.withdrawal_mode = Consensus::BridgeWithdrawalMode::MANAGED_V1;
    Consensus::BridgeManagedWithdrawalPins managed;
    managed.authority_address.fill(0x11);
    managed.vault_runtime_code_hash = *busd.vault_runtime_code_hash;
    managed.withdrawal_rules_version = Consensus::MANAGED_WITHDRAWAL_RULES_VERSION_V1;
    managed.withdrawal_rules_commitment = uint256{uint8_t{7}};
    busd.managed_withdrawal = managed;
    BOOST_REQUIRE(Consensus::BridgeMintParamsReady(busd));
    return busd;
}

/**
 * A regtest-shaped envelope from `v` (anvil 31337, OI-3 verifier and vault
 * code hashes unset, gate profile R). The light-client pins are placeholders
 * until the synthetic beacon exists (BuildEthereum); no V2 light-client
 * record connects before then.
 */
Consensus::BridgeV2Params V2Envelope(const int v)
{
    Consensus::BridgeV2Params out;
    out.activation_height = v;
    out.burn_height = v;
    out.light_client.trusted_checkpoint_root = uint256{uint8_t{8}};
    out.light_client.trusted_checkpoint_slot = 8'192;
    out.light_client.genesis_validators_root = uint256{uint8_t{9}};
    out.light_client.fork_schedule = {{0, {0x00, 0x00, 0x00, 0xb3}}};
    out.light_client.fork_schedule_valid_through_epoch = 1'024;
    out.light_client.min_sync_committee_participants = Consensus::ETHEREUM_SYNC_COMMITTEE_SUPERMAJORITY;
    out.light_client.max_sync_lag_slots = 65'536;
    out.verifier.address = FillAddress(0x66);
    out.verifier.min_bridge_validators = Consensus::MIN_BRIDGE_VALIDATORS_V1;
    out.verifier.max_bridge_validators = Consensus::MAX_PROVEN_BRIDGE_VALIDATORS_V1;
    out.verifier.min_bridge_total_weight = 4;
    out.vault.origin_chain_id = 31'337;
    out.vault.vault_address = FillAddress(0x44);
    out.vault.token_address = FillAddress(0x55);
    out.vault.token_runtime_code_hash = uint256{uint8_t{10}};
    out.vault.origin_deployment_block = 3;
    out.vault.max_deposit_raw = Consensus::BRIDGE_V2_DEFAULT_CAP_RAW;
    out.vault.mint_caps = Consensus::BridgeMintCaps{
        .max_per_block = Consensus::BRIDGE_V2_DEFAULT_CAP_RAW,
        .max_per_epoch = Consensus::BRIDGE_V2_DEFAULT_CAP_RAW,
        .epoch_length_blocks = 1'440,
    };
    out.gate = Consensus::BridgeV2GateParams::RegtestProfileR();
    BOOST_REQUIRE(out.Valid());
    return out;
}

//! A post-London-shaped (16-field) RLP execution header carrying only the
//! fields the bridge reads: parentHash, stateRoot, receiptsRoot, number and
//! timestamp.
std::vector<unsigned char> RlpExecutionHeader(const uint256& parent, const uint64_t number,
                                              const uint256& state_root, const uint256& receipts_root,
                                              const uint64_t timestamp)
{
    const std::vector<unsigned char> empty{RlpBytes(std::span<const unsigned char>{})};
    std::vector<std::vector<unsigned char>> fields(16, empty);
    fields[0] = RlpHash(parent);
    fields[3] = RlpHash(state_root);
    fields[5] = RlpHash(receipts_root);
    fields[8] = bridge::RlpEncodeUint64(number);
    fields[11] = bridge::RlpEncodeUint64(timestamp);
    return bridge::RlpEncodeList(fields);
}

//! What one synthetic execution block commits to.
struct ExecSpec {
    uint256 state_root{};
    uint256 receipts_root{};
    uint64_t timestamp{0};
};

//! Execution blocks EXEC_FIRST.. as the beacon carries them, with the RLP
//! header whose keccak is each block hash.
struct ExecChain {
    std::vector<bridge::ssz::ExecutionPayloadHeader> headers{};
    std::vector<std::vector<unsigned char>> rlp{};

    const bridge::ssz::ExecutionPayloadHeader& At(const uint64_t number) const
    {
        return headers.at(static_cast<size_t>(number - EXEC_FIRST));
    }
};

ExecChain BuildExecChain(const std::vector<ExecSpec>& specs)
{
    ExecChain chain;
    uint256 parent{Keccak(std::vector<unsigned char>{'g', 'e', 'n'})};
    for (size_t i{0}; i < specs.size(); ++i) {
        const uint64_t number{EXEC_FIRST + i};
        std::vector<unsigned char> rlp{RlpExecutionHeader(parent, number, specs[i].state_root,
                                                          specs[i].receipts_root, specs[i].timestamp)};
        bridge::ssz::ExecutionPayloadHeader header;
        header.parent_hash = parent;
        header.state_root = specs[i].state_root;
        header.receipts_root = specs[i].receipts_root;
        header.block_number = number;
        header.gas_limit = 30'000'000;
        header.timestamp = specs[i].timestamp;
        header.block_hash = Keccak(rlp);
        parent = header.block_hash;
        chain.headers.push_back(header);
        chain.rlp.push_back(std::move(rlp));
    }
    return chain;
}

/** A one-receipt trie holding one Vault V2 Deposit log (bridge_state_tests'
 * MakeReceiptFixture layout): its root, its single leaf node and the
 * recipient's B3 script. */
struct DepositReceipt {
    uint256 receipts_root{};
    std::vector<unsigned char> leaf{};
    CScript recipient_script{};
};

DepositReceipt MakeDepositReceipt(const Consensus::BridgeV2VaultEntry& entry, const uint64_t deposit_id,
                                  const uint64_t amount, const unsigned char recipient_fill,
                                  const std::optional<bridge::RecipientV1>& recipient_override = std::nullopt)
{
    std::array<unsigned char, 32> deposit_topic{};
    WriteBE64(deposit_topic.data() + 24, deposit_id);
    std::array<unsigned char, 32> token_topic{};
    std::copy(entry.token_address.begin(), entry.token_address.end(), token_topic.begin() + 12);
    const std::vector<std::vector<unsigned char>> topics{RlpHash(bridge::DepositTopic()), RlpBytes(deposit_topic),
                                                         RlpBytes(token_topic)};

    bridge::RecipientV1 recipient;
    recipient.pubkey_hash.fill(recipient_fill);
    if (recipient_override) recipient = *recipient_override;
    const auto encoded_recipient{bridge::EncodeRecipientV1(recipient)};
    std::array<unsigned char, 64> data{};
    WriteBE64(data.data() + 24, amount);
    std::copy(encoded_recipient.begin(), encoded_recipient.end(), data.begin() + 32);
    const std::vector<unsigned char> log{
        bridge::RlpEncodeList({RlpBytes(entry.vault_address), bridge::RlpEncodeList(topics), RlpBytes(data)})};

    const std::array<unsigned char, 256> bloom{};
    const std::vector<unsigned char> receipt{bridge::RlpEncodeList(
        {bridge::RlpEncodeUint64(1), bridge::RlpEncodeUint64(1), RlpBytes(bloom), bridge::RlpEncodeList({log})})};
    // Key rlp(0) = 0x80, nibbles [8, 0]: an even leaf path 0x20 || 0x80.
    const std::array<unsigned char, 2> leaf_path{0x20, 0x80};
    DepositReceipt out;
    out.leaf = bridge::RlpEncodeList({RlpBytes(leaf_path), RlpBytes(receipt)});
    out.receipts_root = Keccak(out.leaf);
    out.recipient_script = bridge::RecipientV1Script(recipient);
    return out;
}

bool Has(const std::string& text, const std::string& part)
{
    const bool found{text.find(part) != std::string::npos};
    BOOST_CHECK_MESSAGE(found, "'" << text << "' lacks '" << part << "'");
    return found;
}

bool Lacks(const std::string& text, const std::string& part)
{
    const bool absent{text.find(part) == std::string::npos};
    BOOST_CHECK_MESSAGE(absent, "'" << text << "' unexpectedly has '" << part << "'");
    return absent;
}

//! 0x-prefixed raw bytes, the RPCs' Ethereum-facing hex.
std::string EvmHexOf(std::span<const unsigned char> bytes) { return "0x" + HexStr(bytes); }
std::string EvmHexOf(const uint256& value) { return EvmHexOf(std::span<const unsigned char>{value.begin(), 32}); }

/**
 * The eth_getProof results a relayer passes to buildbridgeobservationv2 for
 * `subject` against `world`: one per plan account, in order, with the extra
 * fields an Ethereum node returns (which the RPC ignores).
 */
UniValue EthGetProofJson(const bridge_test::ObservationWorld& world, const uint8_t subject)
{
    const bridge::BridgeEthStateObservationV2 record{world.Observe(subject, uint256::ONE)};
    const auto plan{bridge::ObservationRequests(subject, world.pins)};
    BOOST_REQUIRE_EQUAL(record.accounts.size(), plan.size());
    const auto nodes_json{[](const std::vector<std::vector<unsigned char>>& nodes) {
        UniValue out{UniValue::VARR};
        for (const auto& node : nodes) out.push_back(EvmHexOf(node));
        return out;
    }};
    UniValue out{UniValue::VARR};
    for (size_t i{0}; i < plan.size(); ++i) {
        UniValue result{UniValue::VOBJ};
        result.pushKV("address", EvmHexOf(plan[i].address));
        result.pushKV("balance", "0x0");
        result.pushKV("nonce", "0x1");
        result.pushKV("accountProof", nodes_json(record.accounts[i].account_nodes));
        UniValue storage{UniValue::VARR};
        for (size_t j{0}; j < plan[i].slots.size(); ++j) {
            UniValue slot{UniValue::VOBJ};
            slot.pushKV("key", EvmHexOf(plan[i].slots[j]));
            slot.pushKV("value", "0x0");
            slot.pushKV("proof", nodes_json(record.accounts[i].storage_nodes[j]));
            storage.push_back(std::move(slot));
        }
        result.pushKV("storageProof", std::move(storage));
        out.push_back(std::move(result));
    }
    return out;
}

//! `object` with key `key` replaced (or removed when `replacement` is null).
UniValue WithKey(const UniValue& object, const std::string& key, const UniValue& replacement)
{
    UniValue out{UniValue::VOBJ};
    for (size_t i{0}; i < object.size(); ++i) {
        if (object.getKeys()[i] == key) {
            if (!replacement.isNull()) out.pushKV(key, replacement);
        } else {
            out.pushKV(object.getKeys()[i], object.getValues()[i]);
        }
    }
    return out;
}

//! `array` with element `index` replaced (or removed when `replacement` is null).
UniValue WithElement(const UniValue& array, const size_t index, const UniValue& replacement)
{
    UniValue out{UniValue::VARR};
    for (size_t i{0}; i < array.size(); ++i) {
        if (i != index) {
            out.push_back(array[i]);
        } else if (!replacement.isNull()) {
            out.push_back(replacement);
        }
    }
    return out;
}

struct BridgeV2GateFixture : public FinalityChainFixture {
    int m_bridge_height{0};
    int m_G{0};
    int m_v{0};
    //! Native outpoint funding the chained kind 1, 2 and 9 carriers. It
    //! moves only when a carrier connects (ConfirmLane).
    COutPoint m_lane{};
    CAmount m_lane_value{0};

    // The synthetic Ethereum side (BuildEthereum).
    bridge::VerifierStateV2 m_verifier{};
    std::optional<bridge_test::ObservationWorld> m_healthy{};
    //! A drained vault (invariant R fails), a paused token and a cancelled
    //! verifier, each the subject of one gate row.
    std::optional<bridge_test::ObservationWorld> m_impaired{};
    DepositReceipt m_deposit{};
    //! When set (before BuildEthereum), the deposit pays this recipient
    //! instead of the fill-0x31 key hash nobody holds.
    std::optional<bridge::RecipientV1> m_deposit_recipient{};
    ExecChain m_exec{};
    std::optional<bridge::regtest_beacon::SyntheticBeacon> m_beacon{};

    //! The chain to V - 2 with every regime configured (see file comment).
    void Prepare()
    {
        PrepareFinalityChain(/*min_finality_set=*/1, /*reorg_horizon=*/200, /*with_unbound_c=*/false,
                             /*with_bound_c=*/true, /*with_bound_d=*/true);
        Consensus::Params& c{MutableConsensus()};
        m_bridge_height = m_M + 1;
        m_G = m_M + 10; // GENESIS_HEIGHT, on the V1 grid
        const int fresh_height{m_G + 1};
        m_v = fresh_height + 1;
        c.busd_bridge = ManagedBusd(m_bridge_height);
        c.bridge_withdrawal_activation_height = m_bridge_height;
        c.fresh_lineage = Consensus::FreshLineageParams{
            .activation_height = fresh_height, .max_epoch_extension = 30, .staging_signer = true};
        c.bridge_v2 = V2Envelope(m_v);
        // A bUSD V2 OWNER output needs the colored-asset rules (only the V1
        // bridge asset is exempt). This fixture chain schedules no A2; the
        // flowmesh regtest chain does.
        c.test_only_asset_policies_active = true;
        BOOST_REQUIRE(c.BridgeV2ConfigurationValid());
        BOOST_REQUIRE(Consensus::BridgeRulesActive(m_bridge_height, c));
        BOOST_REQUIRE(Consensus::BridgeWithdrawalRulesActive(m_bridge_height, c));
        m_lane = COutPoint{m_fund_txid, 4};
        m_lane_value = m_fund_value / 16;
        BOOST_REQUIRE(m_lane_value > 1'000 * CARRIER_FEE);
        ProduceTo(m_v - 2, m_vk_a);
    }

    Consensus::BridgeV2Params& V2() { return *MutableConsensus().bridge_v2; }
    const Consensus::Params& Params() { return m_node.chainman->GetConsensus(); }

    //! The bridge index synced to the tip.
    const node::BridgeStateIndex& BridgeIndex()
    {
        LOCK(cs_main);
        Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
        node::BridgeStateTracker& bridge{chainstate.ModernBridgeState()};
        BOOST_REQUIRE(bridge.Sync(chainstate.m_chain, chainstate.m_blockman, Params(), *chainstate.m_chain.Tip()));
        return bridge.Index();
    }

    //! The bridge V2 lineage context for the block on the tip, built exactly
    //! as the four paths build it, handed to `use` under cs_main with the
    //! bridge index.
    template <typename Fn>
    auto WithLineage(Fn&& use)
    {
        LOCK(cs_main);
        Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
        node::BridgeStateTracker& bridge{chainstate.ModernBridgeState()};
        const CBlockIndex& tip{*chainstate.m_chain.Tip()};
        BOOST_REQUIRE(bridge.Sync(chainstate.m_chain, chainstate.m_blockman, Params(), tip));
        std::string reason;
        const auto context{node::MakeBridgeV2LineageContext(chainstate.ModernFinality(), chainstate.m_chain,
                                                            chainstate.m_blockman, Params(), tip, bridge.Index(),
                                                            reason)};
        BOOST_REQUIRE_MESSAGE(context, reason);
        return use(*context, bridge.Index());
    }

    //! The burn gate for the block on the tip at `candidate_time`.
    node::BridgeV2GateResult GateAt(const int64_t candidate_time)
    {
        return WithLineage([&](const node::BridgeV2LineageContext& context, const node::BridgeStateIndex& index) {
            return node::EvaluateBridgeV2BurnGate(&context, index, Params(), Tip()->nHeight + 1, candidate_time);
        });
    }

    //! The fresh checkpoint finalized at the tip (-1 without one).
    int FreshFinalizedHeight()
    {
        return WithLineage([](const node::BridgeV2LineageContext& context, const node::BridgeStateIndex&) {
            return context.fresh && context.fresh->finalized ? context.fresh->finalized->height : -1;
        });
    }

    //! B3's canonical epoch-`e` statement at fresh checkpoint `height`.
    modern::FinalizedBlockV2 Statement(const uint64_t e, const int height)
    {
        return WithLineage([&](const node::BridgeV2LineageContext& context, const node::BridgeStateIndex& index) {
            BOOST_REQUIRE(context.fresh && context.fresh->bootstrapped);
            const auto fb{node::ExpectedFinalizedBlockV2(e, height, *context.fresh, context.hash_at, Params(),
                                                         &index)};
            BOOST_REQUIRE(fb);
            return *fb;
        });
    }

    //! B3's canonical statement at fresh checkpoint `checkpoint`, in the
    //! epoch the next block's projected lineage assigns to it.
    modern::FinalizedBlockV2 CheckpointStatement(const int checkpoint)
    {
        const uint64_t e{WithLineage([&](const node::BridgeV2LineageContext& context, const node::BridgeStateIndex&) {
            BOOST_REQUIRE(context.fresh && context.fresh->bootstrapped);
            const auto epoch{modern::EpochOfHeight(context.fresh->epoch_starts, checkpoint)};
            BOOST_REQUIRE(epoch);
            return *epoch;
        })};
        return Statement(e, checkpoint);
    }

    /**
     * `fb` signed by every member of Set'_`e` (e in {epoch, epoch - 1} of the
     * next block's projected lineage), whatever the statement says, with
     * that committee's size.
     */
    std::pair<modern::FinalityCertificate, uint32_t> CommitteeSigned(const modern::FinalizedBlockV2& fb,
                                                                     const uint64_t e)
    {
        return WithLineage([&](const node::BridgeV2LineageContext& context, const node::BridgeStateIndex&) {
            BOOST_REQUIRE(context.fresh && context.fresh->bootstrapped);
            const auto set{context.fresh->SetForEpoch(e)};
            BOOST_REQUIRE(set);
            const uint256 digest{modern::FinalityDigestV2(m_domain, fb)};
            modern::FinalityCertificate cert;
            cert.signer_bitmap.assign(modern::SignerBitmapBytes(set->Size()), 0);
            std::vector<bls::Signature> signatures;
            for (uint32_t i{0}; i < set->Size(); ++i) {
                const auto& pubkey{set->members->Members()[i].bls_pubkey};
                for (const bls::SecretKey* key : {&m_bls_a, &m_bls_b, &m_bls_c, &m_bls_d}) {
                    if (key->GetPublicKey().Compressed() != pubkey) continue;
                    cert.signer_bitmap[i / 8] |= static_cast<unsigned char>(1U << (i % 8));
                    signatures.push_back(key->Sign(std::span<const unsigned char>(digest.begin(), 32)));
                }
            }
            BOOST_REQUIRE_EQUAL(signatures.size(), set->Size());
            cert.aggregate_sig = bls::AggregateSignatures(signatures)->Compressed();
            return std::pair{cert, static_cast<uint32_t>(set->Size())};
        });
    }

    //! The type-11 record of `fb` signed by every member of Set'_{fb.epoch}.
    CMpaRecord CertificateRecord(const modern::FinalizedBlockV2& fb)
    {
        CMpaRecord record;
        record.payload_type = modern::MPA_TYPE_FINALITY_CERTIFICATE_V2;
        record.payload_version = modern::MPA_VERSION_V1;
        record.payload = modern::EncodeCertificatePayloadV2(fb, CommitteeSigned(fb, fb.epoch).first);
        return record;
    }

    /**
     * The type-11 certificate the next block may carry for fresh checkpoint
     * `checkpoint`: B3's canonical statement signed by every member of the
     * committee the next block's projected lineage assigns to its epoch.
     */
    CMpaRecord FreshCertificate(const int checkpoint) { return CertificateRecord(CheckpointStatement(checkpoint)); }

    //! The certificate the next block carries so C3 never fails by accident:
    //! fresh checkpoints G + 5, G + 15, G + 25, ... at the certificate depth.
    std::optional<CMpaRecord> CertificateDue()
    {
        const int checkpoint{Tip()->nHeight + 1 - SCALED_DEPTH};
        if (checkpoint <= m_G || (checkpoint - m_G) % (2 * SCALED_INTERVAL) != SCALED_INTERVAL) return std::nullopt;
        if (FreshFinalizedHeight() >= checkpoint) return std::nullopt;
        return FreshCertificate(checkpoint);
    }

    //! A modern-PoS block by A on the tip carrying `txs` and, when given, a
    //! type-11 certificate in its coinbase.
    CBlock BuildBlock(std::vector<CMutableTransaction> txs, const std::optional<CMpaRecord>& certificate)
    {
        const CBlockIndex* prev{Tip()};
        const Consensus::ModernPosParams& pos{*Params().modern_pos};
        const auto [w, W] = RoundWeights(prev, m_vk_a);
        BOOST_REQUIRE(w > 0);
        const uint256 seed{SeedFor(prev)};
        int64_t round{-1};
        for (int64_t r{0}; r < 100'000 && round < 0; ++r) {
            const uint256 digest{modern::ModernPosEligibilityDigest(
                Domain(), seed, static_cast<uint32_t>(prev->nHeight + 1), static_cast<uint32_t>(r), m_vk_a)};
            if (modern::ModernPosEligible(digest, w, W, r, pos)) round = r;
        }
        BOOST_REQUIRE(round >= 0);
        CBlock block{BuildPos(prev, m_vk_a, round, 0, 0, std::move(txs))};
        if (certificate) {
            CMutableTransaction coinbase{*block.vtx[0]};
            coinbase.mpa = {*certificate};
            block.vtx[0] = MakeTransactionRef(coinbase);
        }
        if (modern::BlockHasAnyMpa(block)) {
            CMutableTransaction coinbase{*block.vtx[0]};
            coinbase.vout.emplace_back(0, modern::MakePayloadRootCellScript(modern::ComputePayloadRoot(block)));
            block.vtx[0] = MakeTransactionRef(coinbase);
        }
        block.hashMerkleRoot = BlockMerkleRoot(block);
        Sign(block, m_validator_a);
        return block;
    }

    //! Connect the next block with `txs` and any certificate due.
    void ProduceBlock(std::vector<CMutableTransaction> txs = {})
    {
        const int prev_height{Tip()->nHeight};
        const CBlock block{BuildBlock(std::move(txs), CertificateDue())};
        if (block.GetBlockTime() > GetTime()) SetMockTime(block.GetBlockTime());
        const std::string probe{Probe(block)};
        BOOST_REQUIRE_MESSAGE(probe.empty(), "block at " << prev_height + 1 << " invalid: " << probe);
        BOOST_REQUIRE(Submit(block));
        BOOST_REQUIRE_EQUAL(Tip()->nHeight, prev_height + 1);
    }

    void ProduceBlocksTo(const int height)
    {
        while (Tip()->nHeight < height) ProduceBlock();
    }

    //! The exact time of the next block, the time block validation and
    //! block assembly judge it at.
    int64_t NextBlockTime() { return BuildBlock({}, std::nullopt).GetBlockTime(); }

    //! Wall clock = the next block's time, so mempool admission and wallet
    //! prevalidation judge at the same candidate time.
    void AlignClock() { SetMockTime(NextBlockTime()); }

    static CMpaRecord BridgeMpa(const bridge::BridgeRecordV1& record)
    {
        const auto mpa{bridge::MakeBridgeMpaRecord(record, bridge::BRIDGE_MPT_LIMITS_V2)};
        BOOST_REQUIRE(mpa);
        return *mpa;
    }

    //! A native funding input (fund outputs 0-3, 6, 7, 10, 11 carry stakes
    //! and bindings; 4 funds the carrier lane) with a change output after
    //! `outputs`, `record` as MPA.
    CMutableTransaction BridgeTx(const unsigned fund_output, std::vector<CTxOut> outputs, const CMpaRecord& record)
    {
        CMutableTransaction tx;
        tx.version = 2;
        tx.vin.resize(1);
        tx.vin[0].prevout = COutPoint{m_fund_txid, fund_output};
        tx.vout = std::move(outputs);
        tx.vout.emplace_back(m_fund_value / 16 - 10'000, CScript() << OP_TRUE);
        tx.mpa = {record};
        return tx;
    }

    //! Kind 1, 2 and 9 carriers chained from the funding lane, in order.
    std::vector<CMutableTransaction> Carriers(const std::vector<bridge::BridgeRecordV1>& records)
    {
        COutPoint input{m_lane};
        CAmount value{m_lane_value};
        std::vector<CMutableTransaction> out;
        for (const bridge::BridgeRecordV1& record : records) {
            const CMpaRecord mpa{BridgeMpa(record)};
            const auto binding{modern::MakeBridgeBindingOutput(mpa)};
            BOOST_REQUIRE(binding);
            CMutableTransaction tx;
            tx.version = 2;
            tx.vin.resize(1);
            tx.vin[0].prevout = input;
            tx.vout = {*binding, CTxOut{value - CARRIER_FEE, CScript() << OP_TRUE}};
            tx.mpa = {mpa};
            input = COutPoint{tx.GetHash(), 1};
            value = tx.vout[1].nValue;
            out.push_back(std::move(tx));
        }
        return out;
    }
    CMutableTransaction Carrier(const bridge::BridgeRecordV1& record) { return Carriers({record}).front(); }

    void ConfirmLane(const CMutableTransaction& last)
    {
        m_lane = COutPoint{last.GetHash(), 1};
        m_lane_value = last.vout[1].nValue;
    }

    //! Connect `first` and then the carriers of `records` in the next block.
    void ConnectCarriers(const std::vector<bridge::BridgeRecordV1>& records,
                         std::vector<CMutableTransaction> first = {})
    {
        const std::vector<CMutableTransaction> carriers{Carriers(records)};
        first.insert(first.end(), carriers.begin(), carriers.end());
        ProduceBlock(std::move(first));
        ConfirmLane(carriers.back());
    }

    //! A kind 6 burn of BURN_AMOUNT (non-existent) bUSD V2 naming the V2
    //! vault entry of `role` (the launch entry by default), funded by fund
    //! output `fund_output`.
    CMutableTransaction SyntheticBurn(
        const Consensus::BridgeV2EntryRole role = Consensus::BridgeV2EntryRole::LAUNCH,
        const unsigned fund_output = 8)
    {
        const auto asset{modern::ConfiguredBridgeV2AssetId(Params())};
        const auto entry{modern::ConfiguredBridgeV2EntryId(Params(), role)};
        BOOST_REQUIRE(asset && entry);
        bridge::BridgeBurnV1 burn;
        burn.registry_id = *entry;
        burn.burn_output_index = 0;
        burn.raw_amount = BURN_AMOUNT;
        burn.ethereum_recipient.fill(0x99);
        const auto record{bridge::MakeBridgeMpaRecord(
            bridge::BridgeRecordV1{bridge::BridgeRecordKindV1::BRIDGE_BURN, burn})};
        BOOST_REQUIRE(record);
        const auto burned{modern::MakeAssetBurnOutput(*asset, BURN_AMOUNT)};
        const auto binding{modern::MakeBridgeBindingOutput(*record)};
        BOOST_REQUIRE(burned && binding);
        return BridgeTx(fund_output, {*burned, *binding}, *record);
    }

    //! A kind 3 mint naming the V2 vault entry whose last receipt-proof node
    //! is `terminal_size` bytes: over V1's 1,024, within V2's 8,192. The
    //! proof is junk; only the record limits matter before bridge state.
    CMutableTransaction LargeReceiptMint(const size_t terminal_size)
    {
        const auto asset{modern::ConfiguredBridgeV2AssetId(Params())};
        const auto entry{modern::ConfiguredBridgeV2EntryId(Params())};
        BOOST_REQUIRE(asset && entry);
        bridge::BridgeMintV1 mint;
        mint.registry_id = *entry;
        mint.output_index = 0;
        mint.finalized_anchor_hash = uint256{uint8_t{0x21}};
        mint.target_block_number = 10;
        mint.tx_index = 0;
        mint.receipt_log_index = 0;
        mint.ancestry_headers = {std::vector<unsigned char>(600, 0x22)};
        mint.mpt_nodes = {std::vector<unsigned char>(500, 0x23), std::vector<unsigned char>(terminal_size, 0x24)};
        const bridge::BridgeRecordV1 record{bridge::BridgeRecordKindV1::MINT, mint};
        BOOST_REQUIRE(!bridge::MakeBridgeMpaRecord(record)); // V1 limits refuse it
        const CMpaRecord mpa{BridgeMpa(record)};
        CKey key;
        key.MakeNewKey(/*fCompressed=*/true);
        const auto owner{modern::MakeAssetOwnerOutput(
            *asset, 60, GetScriptForDestination(PKHash(key.GetPubKey())))};
        const auto binding{modern::MakeBridgeBindingOutput(mpa)};
        BOOST_REQUIRE(owner && binding);
        return BridgeTx(9, {*owner, *binding}, mpa);
    }

    /**
     * The synthetic Ethereum side, built once the first fresh checkpoint
     * G + 5 is certified. Execution blocks (T0 = the tip's time):
     *   100  bootstrap        healthy state                     T0 - 4,700
     *   101  anchor 1         healthy state, deposit receipt    T0 - 3,600
     *   102  attests 101      healthy state                     T0 - 3,588
     *   103  anchor 2         healthy state                     T0 + 1,150
     *   104  attests 103      healthy state                     T0 + 1,162
     *   105  anchor 3         healthy state                     T0 + 2,400
     *   106  attests 105      healthy state                     T0 + 2,412
     *   107  anchor 4         impaired state (see m_impaired)   T0 + 2,424
     *   108  attests 107      impaired state                    T0 + 2,436
     * Anchor 1's observations close the gate at T0 + 1,201 (t_obs + maximum
     * age + 1); anchor 2 is fresh once they are stale. The light-client pins
     * move from the placeholders to this beacon before any V2 light-client
     * record connects; records name the vault entry, which the pins do not
     * change.
     */
    void BuildEthereum()
    {
        const Consensus::Params& params{Params()};
        const Consensus::BridgeV2Params& v2{*params.bridge_v2};
        const int64_t t0{Tip()->GetBlockTime()};
        const modern::FinalizedBlockV2 statement{Statement(0, m_G + SCALED_INTERVAL)};
        const auto [set0, set1] = WithLineage(
            [](const node::BridgeV2LineageContext& context, const node::BridgeStateIndex&) {
                const auto current{context.fresh->SetForEpoch(0)};
                const auto successor{context.fresh->SuccessorHashForEpoch(0)};
                BOOST_REQUIRE(current && successor);
                return std::pair{current->set_hash, *successor};
            });

        m_verifier = {};
        m_verifier.current_epoch = 0;
        m_verifier.last_rotation_time = t0 - 4'000;
        m_verifier.last_certificate_time = t0 - 3'700;
        m_verifier.latest_bridge_height = statement.height;
        m_verifier.status = bridge::VERIFIER_STATUS_LIVE;
        m_verifier.current_set_hash = set0;
        m_verifier.next_set_hash = set1;
        m_verifier.latest_height = statement.height;
        m_verifier.latest_index = statement.checkpoint_index;
        m_verifier.latest_init_time = t0 - 4'000;
        m_verifier.latest_cert_id = modern::FinalityCertIdV2(statement);
        BOOST_REQUIRE_EQUAL(m_verifier.latest_index, 1U);

        m_healthy.emplace(bridge::ObservationPinsFromParams(v2));
        m_healthy->SetVerifier(m_verifier);
        m_healthy->SetTokenFlags(/*paused=*/false, /*deprecated=*/false, /*vault_blacklisted=*/false);
        m_healthy->SetReserve(/*next_deposit_id=*/DEPOSIT_ID + 1, /*locked=*/DEPOSIT_AMOUNT,
                              /*absorbed_total=*/0, /*balance=*/DEPOSIT_AMOUNT);
        // A forged drain (locked below the proven deposits under the cut), a
        // paused token and a cancelled verifier.
        m_impaired.emplace(*m_healthy);
        m_impaired->SetReserve(DEPOSIT_ID + 1, /*locked=*/10, /*absorbed_total=*/0, /*balance=*/10);
        m_impaired->SetTokenFlags(/*paused=*/true, /*deprecated=*/false, /*vault_blacklisted=*/false);
        bridge::VerifierStateV2 cancelled{m_verifier};
        cancelled.status = bridge::VERIFIER_STATUS_CANCELLED;
        m_impaired->SetVerifier(cancelled);

        m_deposit = MakeDepositReceipt(v2.vault, DEPOSIT_ID, DEPOSIT_AMOUNT, 0x31, m_deposit_recipient);
        const uint256 healthy{m_healthy->StateRoot()};
        const uint256 impaired{m_impaired->StateRoot()};
        const uint256 no_receipts{Keccak(std::vector<unsigned char>{'r'})};
        const uint64_t t{static_cast<uint64_t>(t0)};
        m_exec = BuildExecChain({
            {healthy, no_receipts, t - 4'700},
            {healthy, m_deposit.receipts_root, t - 3'600},
            {healthy, no_receipts, t - 3'588},
            {healthy, no_receipts, t + 1'150},
            {healthy, no_receipts, t + 1'162},
            {healthy, no_receipts, t + 2'400},
            {healthy, no_receipts, t + 2'412},
            {impaired, no_receipts, t + 2'424},
            {impaired, no_receipts, t + 2'436},
        });

        const uint64_t base_slot{BEACON_PERIOD * bridge::SLOTS_PER_PERIOD + 16};
        m_beacon.emplace(bridge::regtest_beacon::SyntheticDomain{
                             bridge::regtest_beacon::DefaultGenesisValidatorsRoot(),
                             bridge::regtest_beacon::DEFAULT_FORK_VERSION, 0},
                         base_slot - EXEC_FIRST, m_exec.headers);
        Consensus::EthereumLightClientPins light;
        light.trusted_checkpoint_root = m_beacon->CheckpointRoot();
        light.trusted_checkpoint_slot = m_beacon->Slot(EXEC_FIRST);
        light.genesis_validators_root = bridge::regtest_beacon::DefaultGenesisValidatorsRoot();
        light.fork_schedule = {{0, bridge::regtest_beacon::DEFAULT_FORK_VERSION}};
        light.fork_schedule_valid_through_epoch = bridge::EpochAtSlot(base_slot) + 1'024;
        light.electra_epoch = 0;
        light.min_sync_committee_participants = Consensus::ETHEREUM_SYNC_COMMITTEE_SUPERMAJORITY;
        light.max_sync_lag_slots = 65'536;
        BOOST_REQUIRE(light.Valid());
        const auto entry_before{modern::ConfiguredBridgeV2EntryId(params)};
        V2().light_client = light;
        BOOST_REQUIRE(V2().Valid());
        BOOST_REQUIRE(modern::ConfiguredBridgeV2EntryId(params) == entry_before);
    }

    uint256 ExecHash(const uint64_t number) const { return m_exec.At(number).block_hash; }
    uint64_t ExecTime(const uint64_t number) const { return m_exec.At(number).timestamp; }

    bridge::BridgeRecordV1 BootstrapRecord() const
    {
        return {bridge::BridgeRecordKindV1::BOOTSTRAP, m_beacon->Bootstrap()};
    }
    //! The update finalizing execution block `number` (attested by number + 1).
    bridge::BridgeRecordV1 UpdateRecord(const uint64_t number) const
    {
        return {bridge::BridgeRecordKindV1::UPDATE, bridge::BridgeUpdateV1{m_beacon->Update(number)}};
    }
    bridge::BridgeRecordV1 ObservationRecord(const bridge_test::ObservationWorld& world, const uint8_t subject,
                                             const uint64_t anchor_number, const uint8_t verdict = 0) const
    {
        return {bridge::BridgeRecordKindV1::ETH_STATE_OBSERVATION,
                world.Observe(subject, ExecHash(anchor_number), verdict)};
    }

    //! The kind 3 mint of the deposit at execution block 101 (fund output 5).
    CMutableTransaction DepositMint()
    {
        const auto asset{modern::ConfiguredBridgeV2AssetId(Params())};
        const auto entry{modern::ConfiguredBridgeV2EntryId(Params())};
        BOOST_REQUIRE(asset && entry);
        const uint64_t target{EXEC_FIRST + 1};
        const bridge::BridgeMintV1 mint{*entry, 0, ExecHash(target), target, 0, 0,
                                        {m_exec.rlp.at(1)}, {m_deposit.leaf}};
        const CMpaRecord mpa{BridgeMpa({bridge::BridgeRecordKindV1::MINT, mint})};
        const auto owner{modern::MakeAssetOwnerOutput(*asset, DEPOSIT_AMOUNT, m_deposit.recipient_script)};
        const auto binding{modern::MakeBridgeBindingOutput(mpa)};
        BOOST_REQUIRE(owner && binding);
        return BridgeTx(5, {*owner, *binding}, mpa);
    }

    //! Mempool admission dry run at the tip: "" or "reason, debug".
    std::string Admit(const CMutableTransaction& tx)
    {
        LOCK(cs_main);
        const MempoolAcceptResult result{
            m_node.chainman->ProcessTransaction(MakeTransactionRef(tx), /*test_accept=*/true)};
        return result.m_result_type == MempoolAcceptResult::ResultType::VALID ? std::string{}
                                                                               : result.m_state.ToString();
    }

    //! Block validity of a block on the tip carrying `tx`: "" or the state.
    std::string BlockWith(const CMutableTransaction& tx)
    {
        CBlock block{BuildPosBlock(m_vk_a, {}, {tx})};
        if (block.GetBlockTime() > GetTime()) SetMockTime(block.GetBlockTime());
        return Probe(block);
    }

    //! Block validity of a block on the tip carrying `txs` in order.
    std::string BlockWithAll(const std::vector<CMutableTransaction>& txs)
    {
        CBlock block{BuildPosBlock(m_vk_a, {}, txs)};
        if (block.GetBlockTime() > GetTime()) SetMockTime(block.GetBlockTime());
        return Probe(block);
    }

    //! The carrier of `record` from the funding lane paying `fee` (never
    //! connected, so the lane does not move).
    CMutableTransaction CarrierPaying(const bridge::BridgeRecordV1& record, const CAmount fee)
    {
        CMutableTransaction tx{Carrier(record)};
        BOOST_REQUIRE(fee > 0 && fee < m_lane_value);
        tx.vout[1].nValue = m_lane_value - fee;
        return tx;
    }

    //! node::interfaces wallet prevalidation (bridgewithdraw,
    //! claimbridgedeposit, submitbridgecarrier).
    interfaces::BridgePrevalidationResult Prevalidate(const CMutableTransaction& tx, std::string& error,
                                                     node::BridgeTxAuthorization& authorization)
    {
        const CBlockIndex* tip{Tip()};
        return m_node.chain->prevalidateBridgeTransaction(CTransaction{tx}, tip->GetBlockHash(),
                                                          tip->nHeight + 1, authorization, error);
    }

    //! node::interfaces wallet deposit status (getbridgedepositrecipient):
    //! the result and its error text.
    std::pair<interfaces::BridgePrevalidationResult, std::string> DepositStatus()
    {
        std::string error;
        const interfaces::BridgePrevalidationResult result{m_node.chain->bridgeV2DepositStatus(error)};
        return {result, error};
    }

    /**
     * node::BlockAssembler for validator A with `tx` placed straight in the
     * mempool. CreateNewBlock runs TestBlockValidity, so it throws if the
     * assembler selected a transaction the block rule refuses. Returns "" and
     * sets `carried` when a template was built, else the exception text. The
     * mempool is emptied again.
     */
    std::string Assemble(const CMutableTransaction& mtx, bool& carried)
    {
        carried = false;
        if (NextBlockTime() > GetTime()) SetMockTime(NextBlockTime());
        const CTransactionRef tx{MakeTransactionRef(mtx)};
        CTxMemPool& pool{*m_node.mempool};
        TryAddToMempool(pool, TestMemPoolEntryHelper{}.Fee(10'000).FromTx(tx));
        BOOST_REQUIRE(WITH_LOCK(pool.cs, return pool.exists(tx->GetWitnessHash())));
        node::BlockAssembler::Options options;
        options.coinbase_output_script = CScript() << OP_TRUE;
        options.include_dummy_extranonce = true;
        options.modern_pos_validator_key = m_vk_a;
        BOOST_REQUIRE(options.test_block_validity);
        std::string error;
        try {
            const auto tmpl{
                node::BlockAssembler(m_node.chainman->ActiveChainstate(), &pool, options).CreateNewBlock()};
            BOOST_REQUIRE(tmpl);
            carried = std::any_of(tmpl->block.vtx.begin(), tmpl->block.vtx.end(), [&](const CTransactionRef& in) {
                return in && in->GetHash() == tx->GetHash();
            });
        } catch (const std::runtime_error& e) {
            error = e.what();
        }
        WITH_LOCK(pool.cs, pool.removeRecursive(*tx, MemPoolRemovalReason::REPLACED));
        BOOST_REQUIRE_EQUAL(WITH_LOCK(pool.cs, return pool.size()), 0U);
        return error;
    }
    /**
     * Assemble with every `(tx, fee)` placed straight in the mempool (each
     * fee is the transaction's real fee, so the coinbase stays valid);
     * `carried[i]` tells whether the template carries entry i. Returns ""
     * or the exception text, and empties the mempool again.
     */
    std::string AssembleAll(const std::vector<std::pair<CMutableTransaction, CAmount>>& entries,
                            std::vector<bool>& carried)
    {
        carried.assign(entries.size(), false);
        if (NextBlockTime() > GetTime()) SetMockTime(NextBlockTime());
        CTxMemPool& pool{*m_node.mempool};
        std::vector<CTransactionRef> refs;
        for (const auto& [mtx, fee] : entries) {
            refs.push_back(MakeTransactionRef(mtx));
            TryAddToMempool(pool, TestMemPoolEntryHelper{}.Fee(fee).FromTx(refs.back()));
            BOOST_REQUIRE(WITH_LOCK(pool.cs, return pool.exists(refs.back()->GetWitnessHash())));
        }
        node::BlockAssembler::Options options;
        options.coinbase_output_script = CScript() << OP_TRUE;
        options.include_dummy_extranonce = true;
        options.modern_pos_validator_key = m_vk_a;
        BOOST_REQUIRE(options.test_block_validity);
        std::string error;
        try {
            const auto tmpl{
                node::BlockAssembler(m_node.chainman->ActiveChainstate(), &pool, options).CreateNewBlock()};
            BOOST_REQUIRE(tmpl);
            for (size_t i{0}; i < refs.size(); ++i) {
                carried[i] = std::any_of(tmpl->block.vtx.begin(), tmpl->block.vtx.end(),
                                         [&](const CTransactionRef& in) {
                                             return in && in->GetHash() == refs[i]->GetHash();
                                         });
            }
        } catch (const std::runtime_error& e) {
            error = e.what();
        }
        for (const CTransactionRef& tx : refs) {
            WITH_LOCK(pool.cs, pool.removeRecursive(*tx, MemPoolRemovalReason::REPLACED));
        }
        BOOST_REQUIRE_EQUAL(WITH_LOCK(pool.cs, return pool.size()), 0U);
        return error;
    }

    /**
     * A kind 9 carrier of `world` at execution block `anchor`, earlier in the
     * same block as the synthetic burn, closes the gate for the burn at
     * `detail` (the per-burn gate): block validation refuses the burn at its
     * position as bad-bridge-state, the same burn before the carrier passes
     * its gate (failing only at asset conservation), the carrier alone is
     * valid, and block assembly carries the carrier and skips the burn.
     * Mempool admission and wallet prevalidation judge one transaction on
     * the connected state, so the case is the block's alone.
     */
    void ExpectSameBlockRecordClosesBurn(const bridge_test::ObservationWorld& world, const uint8_t subject,
                                         const uint64_t anchor, const std::string& detail)
    {
        AlignClock();
        const CMutableTransaction burn{SyntheticBurn()};
        // A carrier fee rate far above the burn's, so the assembler selects
        // the carrier first.
        const CAmount carrier_fee{50 * CARRIER_FEE};
        const CMutableTransaction carrier{CarrierPaying(ObservationRecord(world, subject, anchor), carrier_fee)};
        BOOST_CHECK_EQUAL(BlockWithAll({carrier}), "");
        const std::string after{BlockWithAll({carrier, burn})};
        Has(after, "bad-bridge-state");
        Has(after, "bridge V2 burn gate is not ready at the burn: " + detail);
        const std::string before{BlockWithAll({burn, carrier})};
        Has(before, "bad-asset-transition");
        Lacks(before, "bridge V2 burn gate");
        std::vector<bool> carried;
        const std::string error{AssembleAll({{carrier, carrier_fee}, {burn, 10'000}}, carried)};
        BOOST_CHECK_MESSAGE(error.empty(), "block assembly failed: " << error);
        BOOST_CHECK(carried.at(0));
        BOOST_CHECK(!carried.at(1));
    }

    //! Whether a valid template carries `tx` (false when it skipped it).
    bool TemplateCarries(const CMutableTransaction& tx)
    {
        bool carried{false};
        const std::string error{Assemble(tx, carried)};
        BOOST_CHECK_MESSAGE(error.empty(), "block assembly failed: " << error);
        return carried;
    }

    /** The synthetic burn is refused for `detail` (a gate row and its
     * reason) by mempool admission, block validation and wallet
     * prevalidation, and skipped by block assembly, all at the next block's
     * time. */
    void ExpectBurnClosed(const std::string& detail)
    {
        AlignClock();
        const CMutableTransaction burn{SyntheticBurn()};
        const std::string admitted{Admit(burn)};
        Has(admitted, "bridge-v2-burn-not-ready");
        Has(admitted, "bridge V2 burn gate is not ready: " + detail);
        const std::string block{BlockWith(burn)};
        Has(block, "bridge-v2-burn-not-ready");
        Has(block, "bridge V2 burn gate is not ready: " + detail);
        std::string error;
        node::BridgeTxAuthorization authorization;
        BOOST_CHECK(Prevalidate(burn, error, authorization) == interfaces::BridgePrevalidationResult::REJECTED);
        Has(error, "bridge V2 burn gate is not ready: " + detail);
        BOOST_CHECK(!authorization.withdrawal_v2);
        BOOST_CHECK(!TemplateCarries(burn));
    }

    /** The synthetic burn passes the gate and the reserve checks in all four
     * paths and fails only at asset conservation; wallet prevalidation, which
     * checks no assets, accepts it. */
    void ExpectBurnPassesGate()
    {
        AlignClock();
        const CMutableTransaction burn{SyntheticBurn()};
        const std::string admitted{Admit(burn)};
        Has(admitted, "bad-asset-transition");
        Lacks(admitted, "bridge-v2-burn");
        const std::string block{BlockWith(burn)};
        Has(block, "bad-asset-transition");
        Lacks(block, "bridge-v2-burn");
        std::string error;
        node::BridgeTxAuthorization authorization;
        BOOST_CHECK_MESSAGE(Prevalidate(burn, error, authorization) == interfaces::BridgePrevalidationResult::VALID,
                            error);
        BOOST_REQUIRE(authorization.withdrawal_v2);
        BOOST_CHECK_EQUAL(authorization.withdrawal_v2->withdrawal.amount, BURN_AMOUNT);
        bool carried{false};
        Has(Assemble(burn, carried), "TestBlockValidity failed: bad-asset-transition");
    }

    /** A kind 9 carrier passes all four paths: admitted, valid in a block,
     * prevalidated and carried by block assembly. */
    void ExpectObservationAccepted(const CMutableTransaction& tx)
    {
        AlignClock();
        BOOST_CHECK_EQUAL(Admit(tx), "");
        BOOST_CHECK_EQUAL(BlockWith(tx), "");
        std::string error;
        node::BridgeTxAuthorization authorization;
        BOOST_CHECK_MESSAGE(Prevalidate(tx, error, authorization) == interfaces::BridgePrevalidationResult::VALID,
                            error);
        BOOST_CHECK(!authorization.mint && !authorization.HasWithdrawal());
        BOOST_CHECK(TemplateCarries(tx));
    }

    /** A kind 9 carrier is refused for `detail` by mempool admission, block
     * validation and wallet prevalidation, and skipped by block assembly. */
    void ExpectObservationRefused(const CMutableTransaction& tx, const std::string& detail)
    {
        AlignClock();
        const std::string admitted{Admit(tx)};
        Has(admitted, "bad-bridge-authorization");
        Has(admitted, detail);
        const std::string block{BlockWith(tx)};
        Has(block, "bad-bridge-state");
        Has(block, detail);
        std::string error;
        node::BridgeTxAuthorization authorization;
        BOOST_CHECK(Prevalidate(tx, error, authorization) == interfaces::BridgePrevalidationResult::REJECTED);
        Has(error, detail);
        BOOST_CHECK(!TemplateCarries(tx));
    }

    /**
     * A node RPC called as b3coin-cli calls it: the string arguments go
     * through the client conversion table (rpc/client.cpp), and -rpcdoccheck
     * is on (as in the functional tests) so the result must match the RPC's
     * documentation. An RPC error throws std::runtime_error with its message.
     */
    UniValue Cli(const std::string& method, const std::vector<std::string>& args = {})
    {
        const std::vector<std::string> commands{tableRPC.listCommands()};
        if (std::find(commands.begin(), commands.end(), method) == commands.end()) {
            RegisterBlockchainRPCCommands(tableRPC);
        }
        JSONRPCRequest request;
        request.context = &m_node;
        request.strMethod = method;
        request.params = RPCConvertValues(method, args);
        if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
        // Restores the effective -rpcdoccheck for the suites that follow.
        struct DocCheck {
            const bool before{gArgs.GetBoolArg("-rpcdoccheck", DEFAULT_RPC_DOC_CHECK)};
            DocCheck() { gArgs.ForceSetArg("-rpcdoccheck", "1"); }
            ~DocCheck() { gArgs.ForceSetArg("-rpcdoccheck", before ? "1" : "0"); }
        } doc_check;
        try {
            return tableRPC.execute(request);
        } catch (const UniValue& error) {
            throw std::runtime_error(error.find_value("message").get_str());
        }
    }

    //! The error of an RPC call that must fail ("" and a test failure if it
    //! succeeds).
    std::string CliError(const std::string& method, const std::vector<std::string>& args)
    {
        try {
            Cli(method, args);
        } catch (const std::runtime_error& e) {
            return e.what();
        }
        BOOST_ERROR(method << " unexpectedly succeeded");
        return {};
    }

    //! buildbridgeobservationv2 for `subject` at execution block `anchor`
    //! from `proofs` (eth_getProof results).
    std::vector<std::string> BuildArgs(const uint8_t subject, const uint64_t anchor, const UniValue& proofs) const
    {
        return {util::ToString(int{subject}), EvmHexOf(ExecHash(anchor)), proofs.write()};
    }
    UniValue BuildObservation(const bridge_test::ObservationWorld& world, const uint8_t subject, const uint64_t anchor)
    {
        return Cli("buildbridgeobservationv2", BuildArgs(subject, anchor, EthGetProofJson(world, subject)));
    }

    //! The canonical payload of ObservationRecord(world, subject, anchor, verdict).
    std::string ExpectedPayload(const bridge_test::ObservationWorld& world, const uint8_t subject,
                                const uint64_t anchor, const uint8_t verdict = 0) const
    {
        const auto encoded{bridge::EncodeBridgeRecordV1(ObservationRecord(world, subject, anchor, verdict),
                                                        bridge::BRIDGE_MPT_LIMITS_V2)};
        BOOST_REQUIRE(encoded);
        return HexStr(*encoded);
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(bridge_v2_gate_tests, BridgeV2GateFixture)

BOOST_AUTO_TEST_CASE(gate_observations_and_receipt_limits_in_mempool_blocks_assembly_and_wallet)
{
    Prepare();
    const Consensus::Params& params{Params()};
    const Consensus::BridgeV2GateParams gate{params.bridge_v2->gate};
    BOOST_REQUIRE(gate == Consensus::BridgeV2GateParams::RegtestProfileR());
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, m_v - 2);

    // -- V2 receipt limits (modern/mpa.h CheckTransactionMpa) ---------------
    // A V2 mint whose terminal receipt node needs the V2 limits is malformed
    // for the block at V - 1, both in the mempool and in a block...
    const CMutableTransaction mint{LargeReceiptMint(2'000)};
    {
        std::string error;
        BOOST_CHECK(!modern::CheckTransactionMpa(CTransaction{mint}, params, m_v - 1, error));
        BOOST_CHECK_EQUAL(error, "mpa-malformed-bridge-record");
        error.clear();
        BOOST_CHECK_MESSAGE(modern::CheckTransactionMpa(CTransaction{mint}, params, m_v, error), error);
    }
    Has(Admit(mint), "mpa-malformed-bridge-record");
    Has(BlockWith(mint), "mpa-malformed-bridge-record");

    Produce(m_vk_a);
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, m_v - 1);
    BOOST_REQUIRE(params.BridgeV2RulesActive(m_v) && params.BridgeV2BurnRulesActive(m_v));
    // ... and from V it decodes and reaches bridge state, which refuses it
    // for its own reason (no V2 light client), not as malformed.
    {
        const std::string admitted{Admit(mint)};
        Has(admitted, "bad-bridge-authorization");
        Has(admitted, "bridge mint precedes light-client bootstrap");
        Lacks(admitted, "mpa-malformed");
        const std::string block{BlockWith(mint)};
        Has(block, "bridge mint precedes light-client bootstrap");
        Lacks(block, "mpa-malformed");
    }

    const CMutableTransaction burn{SyntheticBurn()};

    // -- W2 above the next height: bridge-v2-burn-rules-inactive ------------
    V2().burn_height = m_v + 5;
    BOOST_REQUIRE(params.BridgeV2RulesActive(m_v) && !params.BridgeV2BurnRulesActive(m_v));
    Has(Admit(burn), "bridge-v2-burn-rules-inactive");
    Has(BlockWith(burn), "bridge-v2-burn-rules-inactive");
    {
        std::string error;
        node::BridgeTxAuthorization authorization;
        BOOST_CHECK(Prevalidate(burn, error, authorization) ==
                    interfaces::BridgePrevalidationResult::RULES_INACTIVE);
        Has(error, "Bridge V2 withdrawals are disabled");
    }
    BOOST_CHECK(!TemplateCarries(burn));
    V2().burn_height = m_v;

    // -- C2: committees outside the Verifier V2 bounds. The four-member fresh
    // committees miss a five-member minimum. The V1 (managed) readiness would
    // still be READY here, so block assembly and wallet prevalidation must
    // apply the V2 gate themselves.
    V2().verifier.min_bridge_validators = Consensus::MIN_BRIDGE_VALIDATORS_V1 + 1;
    BOOST_REQUIRE(V2().Valid());
    ExpectBurnClosed("C2: current committee member count is outside the bridge bounds");
    // Weight below the verifier's minimum is NOT_READY too.
    V2().verifier.min_bridge_validators = Consensus::MIN_BRIDGE_VALIDATORS_V1;
    V2().verifier.min_bridge_total_weight = 15 + 1 + 10 + 8 + 1;
    ExpectBurnClosed("C2: current committee weight is below the bridge minimum");
    V2().verifier.min_bridge_total_weight = 4;

    // -- C3: committees in bounds, but no fresh checkpoint is certified yet --
    ExpectBurnClosed("C3: fresh finality lineage has no finalized checkpoint");

    // -- B3's fresh certificate for checkpoint G + 5 connects at G + 8 ------
    ProduceBlocksTo(m_G + SCALED_INTERVAL + SCALED_DEPTH);
    BOOST_REQUIRE_EQUAL(FreshFinalizedHeight(), m_G + SCALED_INTERVAL);

    // -- E1: the lineage rows pass, but no VERIFIER_E has been observed -----
    ExpectBurnClosed("E1: no VERIFIER_E observation");

    // -- The V2 light client: kind 1 (block 100), kind 2 (finalizes 101) ----
    BuildEthereum();
    AlignClock();
    BOOST_CHECK_EQUAL(Admit(Carrier(BootstrapRecord())), "");
    ConnectCarriers({BootstrapRecord()});
    ConnectCarriers({UpdateRecord(EXEC_FIRST + 1)});
    {
        const node::BridgeStateIndex& index{BridgeIndex()};
        BOOST_REQUIRE(index.LightClientV2());
        BOOST_CHECK_EQUAL(index.LightClientV2()->finalized_header.execution.block_number, EXEC_FIRST + 1);
        for (const uint64_t number : {EXEC_FIRST, EXEC_FIRST + 1}) {
            const auto anchor{index.AnchorV2(ExecHash(number))};
            BOOST_REQUIRE(anchor);
            BOOST_CHECK(anchor->state_root == m_healthy->StateRoot());
            BOOST_CHECK_EQUAL(anchor->execution_timestamp, ExecTime(number));
        }
    }

    // -- The relayer's view: getbridgev2info and buildbridgeobservationv2 ----
    // Before any observation the gate stops at E1, and the plan names anchor
    // 101 and each subject's accounts and storage keys.
    {
        const UniValue info{Cli("getbridgev2info")};
        const UniValue& gate_info{info.find_value("burn_gate")};
        BOOST_CHECK_EQUAL(gate_info.find_value("status").get_str(), "NOT_READY");
        BOOST_CHECK_EQUAL(gate_info.find_value("row").get_str(), "E1");
        BOOST_CHECK_EQUAL(gate_info.find_value("reason").get_str(), "no VERIFIER_E observation");
        BOOST_CHECK(gate_info.find_value("now_eff").isNum());
        BOOST_CHECK(gate_info.find_value("closes_at").isNull());
        BOOST_CHECK(gate_info.find_value("temporary").isNull());
        BOOST_CHECK(gate_info.find_value("note").isNull());
        // B3 has not closed deposits: OPEN, even though burns wait at E1.
        const UniValue& deposits{info.find_value("deposits")};
        BOOST_CHECK_EQUAL(deposits.find_value("status").get_str(), "OPEN");
        BOOST_CHECK(deposits.find_value("reason").isNull());
        BOOST_CHECK(DepositStatus().first == interfaces::BridgePrevalidationResult::VALID);
        BOOST_CHECK(info.find_value("observations").empty());
        BOOST_CHECK(info.find_value("conflict").isNull());
        const UniValue& reserve{info.find_value("reserve")};
        BOOST_CHECK_EQUAL(reserve.find_value("minted").getInt<int64_t>(), 0);
        BOOST_CHECK_EQUAL(reserve.find_value("reserve").getInt<int64_t>(), 0);
        BOOST_CHECK_EQUAL(reserve.find_value("proven_deposits").getInt<int64_t>(), 0);
        BOOST_CHECK(reserve.find_value("invariant_r_margin").isNull());
        BOOST_CHECK(reserve.find_value("wind_down_headroom").isNull());
        const UniValue& plan{info.find_value("observation_plan")};
        const UniValue& anchor{plan.find_value("anchor")};
        BOOST_CHECK_EQUAL(anchor.find_value("number").getInt<uint64_t>(), EXEC_FIRST + 1);
        BOOST_CHECK_EQUAL(anchor.find_value("hash_evm").get_str(), EvmHexOf(ExecHash(EXEC_FIRST + 1)));
        BOOST_CHECK_EQUAL(anchor.find_value("timestamp").getInt<uint64_t>(), ExecTime(EXEC_FIRST + 1));
        const UniValue& subjects{plan.find_value("subjects")};
        BOOST_REQUIRE_EQUAL(subjects.size(), 3U);
        const bridge::ObservationPins pins{bridge::ObservationPinsFromParams(*params.bridge_v2)};
        for (const uint8_t subject : {SUBJECT_VERIFIER, SUBJECT_RESERVE, SUBJECT_TOKEN}) {
            const UniValue& entry{subjects[subject - 1]};
            BOOST_CHECK_EQUAL(entry.find_value("subject").getInt<int>(), int{subject});
            BOOST_CHECK(entry.find_value("anchor_is_newer").get_bool());
            BOOST_CHECK(entry.find_value("latest_anchor_block").isNull());
            const auto requests{bridge::ObservationRequests(subject, pins)};
            const UniValue& accounts{entry.find_value("accounts")};
            BOOST_REQUIRE_EQUAL(accounts.size(), requests.size());
            for (size_t i{0}; i < requests.size(); ++i) {
                BOOST_CHECK_EQUAL(accounts[i].find_value("address").get_str(), EvmHexOf(requests[i].address));
                const UniValue& slots{accounts[i].find_value("slots")};
                BOOST_REQUIRE_EQUAL(slots.size(), requests[i].slots.size());
                for (size_t j{0}; j < slots.size(); ++j) {
                    BOOST_CHECK_EQUAL(slots[j].get_str(), EvmHexOf(requests[i].slots[j]));
                }
            }
        }
        BOOST_CHECK_EQUAL(subjects[0].find_value("name").get_str(), "VERIFIER_E");
        BOOST_CHECK_EQUAL(subjects[1].find_value("accounts")[1].find_value("role").get_str(), "token");
    }
    // getbridgev2info's forwarders object (route b and d3 factory pins,
    // -rpcdoccheck on): absent without a pin, then exactly the pins, each
    // hash in raw EVM order. The pins move registry_id_evm and never join
    // `unpinned`; the entry id records name stays.
    {
        const UniValue before{Cli("getbridgev2info")};
        BOOST_CHECK(before.find_value("forwarders").isNull());
        const Consensus::BridgeV2Params saved{V2()};
        BOOST_REQUIRE(!saved.deposit_forwarder && !saved.exit_forwarder);
        const uint256 init_code_hash{uint8_t{0x77}};
        const std::string init_code_hash_evm{"0x77" + std::string(62, '0')};
        const Consensus::BridgeV2DepositForwarderPins deposit_pins{FillAddress(0x71), init_code_hash};
        const Consensus::BridgeV2ExitForwarderPins exit_pins{FillAddress(0x72), init_code_hash, 30'110};
        V2().deposit_forwarder = deposit_pins;
        V2().exit_forwarder = exit_pins;
        BOOST_REQUIRE(V2().Valid());
        const UniValue info{Cli("getbridgev2info")};
        const UniValue& forwarders{info.find_value("forwarders")};
        BOOST_REQUIRE(forwarders.isObject());
        BOOST_CHECK_EQUAL(forwarders.size(), 2U);
        const UniValue& deposit{forwarders.find_value("deposit")};
        BOOST_CHECK_EQUAL(deposit.size(), 2U);
        BOOST_CHECK_EQUAL(deposit.find_value("factory").get_str(), EvmHexOf(FillAddress(0x71)));
        BOOST_CHECK_EQUAL(deposit.find_value("init_code_hash").get_str(), init_code_hash_evm);
        const UniValue& exit_json{forwarders.find_value("exit")};
        BOOST_CHECK_EQUAL(exit_json.size(), 3U);
        BOOST_CHECK_EQUAL(exit_json.find_value("factory").get_str(), EvmHexOf(FillAddress(0x72)));
        BOOST_CHECK_EQUAL(exit_json.find_value("init_code_hash").get_str(), init_code_hash_evm);
        BOOST_CHECK_EQUAL(exit_json.find_value("dst_eid").getInt<uint32_t>(), 30'110U);
        BOOST_CHECK_EQUAL(info.find_value("unpinned").write(), before.find_value("unpinned").write());
        BOOST_CHECK_EQUAL(info.find_value("vault_entry_id_evm").get_str(),
                          before.find_value("vault_entry_id_evm").get_str());
        BOOST_CHECK(info.find_value("registry_id_evm").get_str() != before.find_value("registry_id_evm").get_str());
        // Each pin is optional on its own.
        V2().deposit_forwarder.reset();
        const UniValue exit_only{Cli("getbridgev2info").find_value("forwarders")};
        BOOST_CHECK(exit_only.find_value("deposit").isNull());
        BOOST_CHECK_EQUAL(exit_only.find_value("exit").write(), exit_json.write());
        V2().deposit_forwarder = deposit_pins;
        V2().exit_forwarder.reset();
        const UniValue deposit_only{Cli("getbridgev2info").find_value("forwarders")};
        BOOST_CHECK(deposit_only.find_value("exit").isNull());
        BOOST_CHECK_EQUAL(deposit_only.find_value("deposit").write(), deposit.write());
        V2() = saved;
        BOOST_CHECK(Cli("getbridgev2info").find_value("forwarders").isNull());
    }
    // Each subject built from eth_getProof results at anchor 101 is exactly
    // the record the consensus checks below connect, with B3's verdict (0).
    for (const uint8_t subject : {SUBJECT_VERIFIER, SUBJECT_RESERVE, SUBJECT_TOKEN}) {
        const UniValue built{BuildObservation(*m_healthy, subject, EXEC_FIRST + 1)};
        BOOST_CHECK_EQUAL(built.find_value("payload_hex").get_str(),
                          ExpectedPayload(*m_healthy, subject, EXEC_FIRST + 1));
        BOOST_CHECK_EQUAL(built.find_value("subject").getInt<int>(), int{subject});
        BOOST_CHECK_EQUAL(built.find_value("verdict").getInt<int>(), 0);
        BOOST_CHECK_EQUAL(built.find_value("anchor_block").getInt<uint64_t>(), EXEC_FIRST + 1);
        BOOST_CHECK_EQUAL(built.find_value("anchor_hash_evm").get_str(), EvmHexOf(ExecHash(EXEC_FIRST + 1)));
        BOOST_CHECK_EQUAL(built.find_value("t_obs").getInt<int64_t>(), static_cast<int64_t>(ExecTime(EXEC_FIRST + 1)));
        BOOST_CHECK_EQUAL(built.find_value("next_height").getInt<int>(), Tip()->nHeight + 1);
    }
    {
        const UniValue values{BuildObservation(*m_healthy, SUBJECT_VERIFIER, EXEC_FIRST + 1).find_value("values")};
        BOOST_CHECK_EQUAL(values.find_value("status_name").get_str(), "LIVE");
        BOOST_CHECK_EQUAL(values.find_value("current_epoch").getInt<uint64_t>(), 0U);
        BOOST_CHECK_EQUAL(values.find_value("latest_index").getInt<uint64_t>(), m_verifier.latest_index);
        BOOST_CHECK_EQUAL(values.find_value("latest_height").getInt<uint64_t>(), m_verifier.latest_height);
        BOOST_CHECK_EQUAL(values.find_value("last_rotation_time").getInt<int64_t>(), m_verifier.last_rotation_time);
        BOOST_CHECK_EQUAL(values.find_value("current_set_hash").get_str(), EvmHexOf(m_verifier.current_set_hash));
        BOOST_CHECK_EQUAL(values.find_value("next_set_hash").get_str(), EvmHexOf(m_verifier.next_set_hash));
        BOOST_CHECK_EQUAL(values.find_value("latest_cert_id").get_str(), EvmHexOf(m_verifier.latest_cert_id));
        BOOST_CHECK(values.find_value("locked").isNull());
        const UniValue reserve{BuildObservation(*m_healthy, SUBJECT_RESERVE, EXEC_FIRST + 1).find_value("values")};
        BOOST_CHECK_EQUAL(reserve.find_value("next_deposit_id").getInt<uint64_t>(), DEPOSIT_ID + 1);
        BOOST_CHECK_EQUAL(reserve.find_value("locked").getInt<int64_t>(), DEPOSIT_AMOUNT);
        BOOST_CHECK_EQUAL(reserve.find_value("balance").getInt<int64_t>(), DEPOSIT_AMOUNT);
        BOOST_CHECK_EQUAL(reserve.find_value("absorbed_total").getInt<int64_t>(), 0);
        BOOST_CHECK_EQUAL(reserve.find_value("token_code_hash").get_str(),
                          EvmHexOf(params.bridge_v2->vault.token_runtime_code_hash));
        const UniValue token{BuildObservation(*m_healthy, SUBJECT_TOKEN, EXEC_FIRST + 1).find_value("values")};
        BOOST_CHECK(!token.find_value("paused").get_bool());
        BOOST_CHECK(!token.find_value("deprecated").get_bool());
        BOOST_CHECK(!token.find_value("vault_blacklisted").get_bool());
        BOOST_CHECK_EQUAL(token.find_value("owner").get_str(), "0x" + std::string(40, '6'));
    }
    // Refusals: argument shape, the plan, the anchor and the proofs.
    {
        const UniValue verifier_proofs{EthGetProofJson(*m_healthy, SUBJECT_VERIFIER)};
        const auto refused{[&](const uint8_t subject, const uint64_t anchor, const UniValue& proofs) {
            return CliError("buildbridgeobservationv2", BuildArgs(subject, anchor, proofs));
        }};
        Has(CliError("buildbridgeobservationv2", {"4", EvmHexOf(ExecHash(EXEC_FIRST + 1)), "[]"}),
            "subject must be 1 (VERIFIER_E), 2 (RESERVE_E) or 3 (TOKEN_FLAGS)");
        Has(CliError("buildbridgeobservationv2", {"1", "0x1234", verifier_proofs.write()}),
            "anchor_hash_evm must contain exactly 32 hexadecimal bytes");
        Has(refused(SUBJECT_VERIFIER, EXEC_FIRST + 1, EthGetProofJson(*m_healthy, SUBJECT_RESERVE)),
            "proofs holds 2 eth_getProof results; this subject's plan has 1 accounts");
        Has(refused(SUBJECT_VERIFIER, EXEC_FIRST + 1, EthGetProofJson(*m_healthy, SUBJECT_TOKEN)),
            "proofs[0].address is not the plan's verifier");
        const UniValue& account{verifier_proofs[0]};
        const UniValue& storage{account.find_value("storageProof")};
        Has(refused(SUBJECT_VERIFIER, EXEC_FIRST + 1,
                    WithElement(verifier_proofs, 0,
                                WithKey(account, "storageProof", WithElement(storage, 4, UniValue{})))),
            "proofs[0].storageProof proves 4 slots; the plan's verifier needs 5");
        Has(refused(SUBJECT_VERIFIER, EXEC_FIRST + 1,
                    WithElement(verifier_proofs, 0,
                                WithKey(account, "storageProof",
                                        WithElement(storage, 1, WithKey(storage[1], "key", UniValue{"0x3"}))))),
            "proofs[0].storageProof[1].key is not the plan's slot");
        // A short quantity key names the same slot: slot 0 as "0x0".
        const UniValue short_key{WithElement(
            verifier_proofs, 0,
            WithKey(account, "storageProof", WithElement(storage, 0, WithKey(storage[0], "key", UniValue{"0x0"}))))};
        BOOST_CHECK_EQUAL(Cli("buildbridgeobservationv2", BuildArgs(SUBJECT_VERIFIER, EXEC_FIRST + 1, short_key))
                              .find_value("payload_hex")
                              .get_str(),
                          ExpectedPayload(*m_healthy, SUBJECT_VERIFIER, EXEC_FIRST + 1));
        // A tampered account node, and proofs of another state (the drained
        // vault) against anchor 101's root, do not verify.
        const UniValue& account_nodes{account.find_value("accountProof")};
        std::string tampered{account_nodes[0].get_str()};
        tampered.back() = tampered.back() == '0' ? '1' : '0';
        Has(refused(SUBJECT_VERIFIER, EXEC_FIRST + 1,
                    WithElement(verifier_proofs, 0,
                                WithKey(account, "accountProof", WithElement(account_nodes, 0, UniValue{tampered})))),
            "The proofs do not verify against the anchor: observation verifier account proof");
        Has(refused(SUBJECT_RESERVE, EXEC_FIRST + 1, EthGetProofJson(*m_impaired, SUBJECT_RESERVE)),
            "The proofs do not verify against the anchor");
        // Execution block 103 is not a retained anchor yet.
        Has(refused(SUBJECT_VERIFIER, EXEC_FIRST + 3, verifier_proofs),
            "anchor_hash_evm is not a retained bridge V2 execution anchor on the active chain");
        // Every chain without a bridge V2 envelope (mainnet, testnet) refuses
        // both RPCs.
        const Consensus::BridgeV2Params saved{*params.bridge_v2};
        MutableConsensus().bridge_v2.reset();
        Has(refused(SUBJECT_VERIFIER, EXEC_FIRST + 1, verifier_proofs), "Bridge V2 is not configured on this chain");
        Has(CliError("getbridgev2info", {}), "Bridge V2 is not configured on this chain");
        MutableConsensus().bridge_v2 = saved;
    }

    // -- Kind 9 VERIFIER_E verdicts in all four paths -----------------------
    // B3's verdict for this verifier is 0: its sets and latestCertId are
    // B3's. A carrier recording another verdict is invalid everywhere, and
    // block assembly skips it while still building a valid block.
    ExpectObservationRefused(Carrier(ObservationRecord(*m_healthy, SUBJECT_VERIFIER, EXEC_FIRST + 1,
                                                       bridge::ETH_OBSERVATION_VERDICT_SETS_MISMATCH)),
                             WRONG_VERDICT);
    ExpectObservationRefused(Carrier(ObservationRecord(*m_healthy, SUBJECT_VERIFIER, EXEC_FIRST + 1,
                                                       bridge::ETH_OBSERVATION_VERDICT_CERT_CONFLICT)),
                             WRONG_VERDICT);
    // The correct verdict is judged on the lineage context in every path;
    // block assembly carries it (without a context it would skip it).
    ExpectObservationAccepted(Carrier(ObservationRecord(*m_healthy, SUBJECT_VERIFIER, EXEC_FIRST + 1)));

    // -- The deposit mint and VERIFIER_E connect: E7 is the next row --------
    ConnectCarriers({ObservationRecord(*m_healthy, SUBJECT_VERIFIER, EXEC_FIRST + 1)}, {DepositMint()});
    {
        const node::BridgeStateIndex& index{BridgeIndex()};
        BOOST_CHECK_EQUAL(index.ReserveLedgerV2().minted, DEPOSIT_AMOUNT);
        BOOST_CHECK_EQUAL(index.ProvenDepositsV2().size(), 1U);
        const auto verifier{index.LatestObservationV2(SUBJECT_VERIFIER)};
        BOOST_REQUIRE(verifier);
        BOOST_CHECK_EQUAL(int{verifier->verdict}, 0);
        BOOST_CHECK_EQUAL(verifier->t_obs, static_cast<int64_t>(ExecTime(EXEC_FIRST + 1)));
        BOOST_CHECK(std::get<bridge::VerifierStateV2>(verifier->state).latest_cert_id == m_verifier.latest_cert_id);
    }
    // buildbridgeobservationv2 runs the next block's bridge checks, so it
    // refuses a VERIFIER_E no newer than the connected one.
    Has(CliError("buildbridgeobservationv2",
                 BuildArgs(SUBJECT_VERIFIER, EXEC_FIRST + 1, EthGetProofJson(*m_healthy, SUBJECT_VERIFIER))),
        "The next block would refuse this observation: bridge observation anchor is not newer than the "
        "subject's latest observation");
    AlignClock();
    {
        const std::string admitted{Admit(burn)};
        Has(admitted, "bridge-v2-burn-not-ready");
        Has(admitted, "E7: no TOKEN_FLAGS observation");
    }

    // -- TOKEN_FLAGS and RESERVE_E connect: the gate is READY ---------------
    ConnectCarriers({ObservationRecord(*m_healthy, SUBJECT_TOKEN, EXEC_FIRST + 1),
                     ObservationRecord(*m_healthy, SUBJECT_RESERVE, EXEC_FIRST + 1)});
    BOOST_CHECK(!BridgeIndex().ConflictV2());
    ExpectBurnPassesGate();
    const int64_t t_obs{static_cast<int64_t>(ExecTime(EXEC_FIRST + 1))};
    const int64_t closes_at{t_obs + gate.observation_max_age_seconds + 1};
    {
        const node::BridgeV2GateResult result{GateAt(NextBlockTime())};
        BOOST_CHECK(result.readiness == node::BridgeBurnReadiness::READY);
        BOOST_REQUIRE(result.closes_at);
        BOOST_CHECK_EQUAL(*result.closes_at, closes_at);
        BOOST_CHECK_EQUAL(result.now_eff, NextBlockTime()); // the B3 time leads L1's
        BOOST_REQUIRE(closes_at - NextBlockTime() > gate.mempool_close_margin_seconds);
    }
    {
        // getbridgev2info reports the same READY gate, the three
        // observations and the reserve ledger with its bounds: invariant R
        // holds exactly (locked 60 = P_pre 60) and burn check 3 admits the
        // whole balance (B 60 >= locked, so no margin).
        const UniValue info{Cli("getbridgev2info")};
        const UniValue& gate_info{info.find_value("burn_gate")};
        BOOST_CHECK_EQUAL(gate_info.find_value("status").get_str(), "READY");
        BOOST_CHECK(gate_info.find_value("row").isNull());
        BOOST_CHECK_EQUAL(gate_info.find_value("reason").get_str(), "");
        BOOST_CHECK_EQUAL(gate_info.find_value("closes_at").getInt<int64_t>(), closes_at);
        BOOST_CHECK_EQUAL(gate_info.find_value("now_eff").getInt<int64_t>(), NextBlockTime());
        const UniValue& observations{info.find_value("observations")};
        BOOST_CHECK_EQUAL(observations.size(), 3U);
        const UniValue& verifier{observations.find_value("verifier_e")};
        BOOST_CHECK_EQUAL(verifier.find_value("name").get_str(), "VERIFIER_E");
        BOOST_CHECK_EQUAL(verifier.find_value("t_obs").getInt<int64_t>(), t_obs);
        BOOST_CHECK_EQUAL(verifier.find_value("anchor_block").getInt<uint64_t>(), EXEC_FIRST + 1);
        BOOST_CHECK_EQUAL(verifier.find_value("anchor_hash_evm").get_str(), EvmHexOf(ExecHash(EXEC_FIRST + 1)));
        BOOST_CHECK_EQUAL(verifier.find_value("verdict").getInt<int>(), 0);
        BOOST_CHECK_EQUAL(verifier.find_value("values").find_value("latest_cert_id").get_str(),
                          EvmHexOf(m_verifier.latest_cert_id));
        BOOST_CHECK_EQUAL(observations.find_value("reserve_e").find_value("values").find_value("locked").getInt<int64_t>(),
                          DEPOSIT_AMOUNT);
        BOOST_CHECK(!observations.find_value("token_flags").find_value("values").find_value("paused").get_bool());
        BOOST_CHECK_EQUAL(observations.find_value("token_flags").find_value("connected_height").getInt<int>(),
                          Tip()->nHeight);
        const UniValue& reserve{info.find_value("reserve")};
        BOOST_CHECK_EQUAL(reserve.find_value("minted").getInt<int64_t>(), DEPOSIT_AMOUNT);
        BOOST_CHECK_EQUAL(reserve.find_value("burned").getInt<int64_t>(), 0);
        BOOST_CHECK_EQUAL(reserve.find_value("credited").getInt<int64_t>(), 0);
        BOOST_CHECK_EQUAL(reserve.find_value("transit").getInt<int64_t>(), 0);
        BOOST_CHECK_EQUAL(reserve.find_value("reserve").getInt<int64_t>(), DEPOSIT_AMOUNT);
        BOOST_CHECK_EQUAL(reserve.find_value("proven_deposits").getInt<int64_t>(), 1);
        BOOST_CHECK_EQUAL(reserve.find_value("invariant_r_margin").getInt<int64_t>(), 0);
        BOOST_CHECK_EQUAL(reserve.find_value("wind_down_headroom").getInt<int64_t>(), DEPOSIT_AMOUNT);
        BOOST_CHECK(info.find_value("conflict").isNull());
        for (const UniValue& entry : info.find_value("observation_plan").find_value("subjects").getValues()) {
            BOOST_CHECK_EQUAL(entry.find_value("latest_anchor_block").getInt<uint64_t>(), EXEC_FIRST + 1);
            BOOST_CHECK(!entry.find_value("anchor_is_newer").get_bool());
        }
    }

    // -- The mempool close margin (policy only) -----------------------------
    // Just outside the margin the mempool still takes the burn up to asset
    // conservation. Within it, and at an age exactly at the limit (where the
    // gate itself is still READY), mempool admission and wallet
    // prevalidation refuse the burn as closing. Block validation at the next
    // block's own time is unaffected.
    SetMockTime(closes_at - gate.mempool_close_margin_seconds - 1);
    Has(Admit(burn), "bad-asset-transition");
    for (const int64_t now : {closes_at - gate.mempool_close_margin_seconds, closes_at - 1}) {
        SetMockTime(now);
        const std::string admitted{Admit(burn)};
        Has(admitted, "bridge-v2-burn-gate-closing");
        Lacks(admitted, "bridge-v2-burn-not-ready");
        std::string error;
        node::BridgeTxAuthorization authorization;
        BOOST_CHECK(Prevalidate(burn, error, authorization) == interfaces::BridgePrevalidationResult::REJECTED);
        Has(error, "within the " + util::ToString(gate.mempool_close_margin_seconds) + " s mempool margin");
    }
    BOOST_REQUIRE(NextBlockTime() < closes_at - gate.mempool_close_margin_seconds);
    Has(BlockWith(burn), "bad-asset-transition");

    // -- Stale observations: t_obs + maximum age + 1 ------------------------
    // Mempool admission and wallet prevalidation judge at the wall clock...
    SetMockTime(closes_at);
    {
        const std::string admitted{Admit(burn)};
        Has(admitted, "bridge-v2-burn-not-ready");
        Has(admitted, "E1: VERIFIER_E observation is stale");
        std::string error;
        node::BridgeTxAuthorization authorization;
        BOOST_CHECK(Prevalidate(burn, error, authorization) == interfaces::BridgePrevalidationResult::REJECTED);
        Has(error, "E1: VERIFIER_E observation is stale");
    }
    {
        const UniValue gate_info{Cli("getbridgev2info").find_value("burn_gate")};
        BOOST_CHECK_EQUAL(gate_info.find_value("status").get_str(), "NOT_READY");
        BOOST_CHECK_EQUAL(gate_info.find_value("row").get_str(), "E1");
        BOOST_CHECK_EQUAL(gate_info.find_value("reason").get_str(), "VERIFIER_E observation is stale");
        BOOST_CHECK_EQUAL(gate_info.find_value("now_eff").getInt<int64_t>(), closes_at);
        BOOST_CHECK(gate_info.find_value("closes_at").isNull());
    }
    // ... and block validation and block assembly at the block's time, once
    // the chain reaches it (fresh certificates keep C3 met meanwhile).
    while (NextBlockTime() < closes_at) ProduceBlock();
    BOOST_REQUIRE(Tip()->GetBlockTime() < closes_at);
    ExpectBurnClosed("E1: VERIFIER_E observation is stale");

    // -- Observations against a newer anchor reopen the gate ----------------
    ConnectCarriers({UpdateRecord(EXEC_FIRST + 3)});
    BOOST_REQUIRE(BridgeIndex().AnchorV2(ExecHash(EXEC_FIRST + 3)));
    // A wrong verdict is refused against the new anchor too, the right one
    // passes, and an observation no newer than its subject's latest is
    // refused.
    ExpectObservationRefused(Carrier(ObservationRecord(*m_healthy, SUBJECT_VERIFIER, EXEC_FIRST + 3,
                                                       bridge::ETH_OBSERVATION_VERDICT_SETS_MISMATCH)),
                             WRONG_VERDICT);
    ExpectObservationAccepted(Carrier(ObservationRecord(*m_healthy, SUBJECT_VERIFIER, EXEC_FIRST + 3)));
    ExpectObservationRefused(Carrier(ObservationRecord(*m_healthy, SUBJECT_TOKEN, EXEC_FIRST + 1)),
                             "bridge observation anchor is not newer than the subject's latest observation");
    ConnectCarriers({ObservationRecord(*m_healthy, SUBJECT_VERIFIER, EXEC_FIRST + 3),
                     ObservationRecord(*m_healthy, SUBJECT_TOKEN, EXEC_FIRST + 3),
                     ObservationRecord(*m_healthy, SUBJECT_RESERVE, EXEC_FIRST + 3)});
    ExpectBurnPassesGate();

    // -- C3: finality lagging beyond the limit closes the gate --------------
    {
        const int lag{Tip()->nHeight + 1 - FreshFinalizedHeight()};
        BOOST_REQUIRE(lag > 1 && lag <= gate.max_finality_lag_blocks);
        V2().gate.max_finality_lag_blocks = lag - 1;
        BOOST_REQUIRE(V2().Valid());
        ExpectBurnClosed("C3: finality lags the candidate height by more than the limit");
        V2().gate.max_finality_lag_blocks = lag;
        BOOST_CHECK(GateAt(NextBlockTime()).readiness == node::BridgeBurnReadiness::READY);
        V2().gate = gate;
    }

    // -- E8 alone stale: VERIFIER_E and TOKEN_FLAGS move to anchor 105 while
    // RESERVE_E stays at anchor 103 ---------------------------------------
    ConnectCarriers({UpdateRecord(EXEC_FIRST + 5)});
    ConnectCarriers({ObservationRecord(*m_healthy, SUBJECT_VERIFIER, EXEC_FIRST + 5),
                     ObservationRecord(*m_healthy, SUBJECT_TOKEN, EXEC_FIRST + 5)});
    ExpectBurnPassesGate();
    {
        // A maximum age between the two observation ages leaves only
        // RESERVE_E stale, so E8 is the first row to fail in all four paths.
        AlignClock();
        const int64_t now_eff{GateAt(NextBlockTime()).now_eff};
        const int64_t reserve_age{now_eff - static_cast<int64_t>(ExecTime(EXEC_FIRST + 3))};
        const int64_t fresh_age{now_eff - static_cast<int64_t>(ExecTime(EXEC_FIRST + 5))};
        BOOST_REQUIRE(fresh_age >= 0 && fresh_age < reserve_age - 1);
        V2().gate.observation_max_age_seconds = reserve_age - 1;
        BOOST_REQUIRE(V2().Valid());
        ExpectBurnClosed("E8: RESERVE_E observation is stale");
        V2().gate = gate;
    }
    // A fresh RESERVE_E at anchor 105 reopens the gate.
    ConnectCarriers({ObservationRecord(*m_healthy, SUBJECT_RESERVE, EXEC_FIRST + 5)});
    ExpectBurnPassesGate();

    // -- A kind 9 record earlier in the same block closes the gate for every
    // later burn of that block (the per-burn gate) -------------------------
    ConnectCarriers({UpdateRecord(EXEC_FIRST + 7)});
    BOOST_REQUIRE(BridgeIndex().AnchorV2(ExecHash(EXEC_FIRST + 7)));
    ExpectBurnPassesGate();
    ExpectSameBlockRecordClosesBurn(*m_impaired, SUBJECT_TOKEN, EXEC_FIRST + 7, "E7: token is paused");
    ExpectSameBlockRecordClosesBurn(*m_impaired, SUBJECT_RESERVE, EXEC_FIRST + 7,
                                    "C4: sticky E8 conflict: the vault failed invariant R");
    ExpectSameBlockRecordClosesBurn(*m_impaired, SUBJECT_VERIFIER, EXEC_FIRST + 7, "C1: verifier is cancelled");
    BOOST_CHECK(!BridgeIndex().ConflictV2());
    ExpectBurnPassesGate();

    // -- E7: a connected TOKEN_FLAGS showing a paused token ------------------
    ExpectObservationAccepted(Carrier(ObservationRecord(*m_impaired, SUBJECT_TOKEN, EXEC_FIRST + 7)));
    ConnectCarriers({ObservationRecord(*m_impaired, SUBJECT_TOKEN, EXEC_FIRST + 7)});
    ExpectBurnClosed("E7: token is paused");

    // -- E8: a RESERVE_E failing invariant R sets the sticky conflict --------
    // The record itself is valid in every path: it is evidence.
    ExpectObservationAccepted(Carrier(ObservationRecord(*m_impaired, SUBJECT_RESERVE, EXEC_FIRST + 7)));
    // The relayer builds the same evidence record.
    {
        const UniValue built{BuildObservation(*m_impaired, SUBJECT_RESERVE, EXEC_FIRST + 7)};
        BOOST_CHECK_EQUAL(built.find_value("payload_hex").get_str(),
                          ExpectedPayload(*m_impaired, SUBJECT_RESERVE, EXEC_FIRST + 7));
        BOOST_CHECK_EQUAL(built.find_value("values").find_value("locked").getInt<int64_t>(), 10);
    }
    ConnectCarriers({ObservationRecord(*m_impaired, SUBJECT_RESERVE, EXEC_FIRST + 7)});
    {
        const auto conflict{BridgeIndex().ConflictV2()};
        BOOST_REQUIRE(conflict);
        BOOST_CHECK_EQUAL(int{conflict->reason}, int{bridge::BRIDGE_V2_CONFLICT_E8});
        BOOST_CHECK_EQUAL(conflict->height, Tip()->nHeight);
        BOOST_CHECK_EQUAL(conflict->anchor_block, EXEC_FIRST + 7);
    }
    {
        // getbridgev2info shows the conflict and C4, and the negative
        // invariant R margin: locked 10 against P_pre 60. Burn check 3 still
        // admits up to the observed balance (B 10 = locked, no margin).
        const UniValue info{Cli("getbridgev2info")};
        const UniValue& conflict{info.find_value("conflict")};
        BOOST_CHECK_EQUAL(conflict.find_value("reason").get_str(), "E8");
        BOOST_CHECK_EQUAL(conflict.find_value("height").getInt<int>(), Tip()->nHeight);
        BOOST_CHECK_EQUAL(conflict.find_value("block").get_str(), Tip()->GetBlockHash().GetHex());
        BOOST_CHECK_EQUAL(conflict.find_value("anchor_block").getInt<uint64_t>(), EXEC_FIRST + 7);
        BOOST_CHECK(conflict.find_value("evidence_cert_id_evm").isNull());
        BOOST_CHECK(conflict.find_value("evidence_checkpoint_index").isNull());
        BOOST_CHECK(conflict.find_value("evidence_epoch").isNull());
        const UniValue& gate_info{info.find_value("burn_gate")};
        BOOST_CHECK_EQUAL(gate_info.find_value("status").get_str(), "NOT_READY");
        BOOST_CHECK_EQUAL(gate_info.find_value("row").get_str(), "C4");
        BOOST_CHECK_EQUAL(gate_info.find_value("reason").get_str(), "sticky E8 conflict: the vault failed invariant R");
        // The E8 conflict closes deposits for good too.
        const std::string e8_closed{"sticky E8 conflict at height " + util::ToString(Tip()->nHeight)};
        const UniValue& deposits{info.find_value("deposits")};
        BOOST_CHECK_EQUAL(deposits.find_value("status").get_str(), "CLOSED_FOR_GOOD");
        BOOST_CHECK_EQUAL(deposits.find_value("reason").get_str(), e8_closed);
        const auto [status, error] = DepositStatus();
        BOOST_CHECK(status == interfaces::BridgePrevalidationResult::REJECTED);
        BOOST_CHECK_EQUAL(error, e8_closed);
        const UniValue& reserve{info.find_value("reserve")};
        BOOST_CHECK_EQUAL(reserve.find_value("invariant_r_margin").getInt<int64_t>(), 10 - DEPOSIT_AMOUNT);
        BOOST_CHECK_EQUAL(reserve.find_value("wind_down_headroom").getInt<int64_t>(), 10);
        BOOST_CHECK_EQUAL(info.find_value("observations").find_value("reserve_e").find_value("anchor_block").getInt<uint64_t>(),
                          EXEC_FIRST + 7);
    }
    ExpectBurnClosed("C4: sticky E8 conflict: the vault failed invariant R");

    // -- C1: a connected VERIFIER_E showing a cancelled verifier (status not
    // LIVE, so its verdict is 0) impairs the entry, at any age -------------
    ExpectObservationAccepted(Carrier(ObservationRecord(*m_impaired, SUBJECT_VERIFIER, EXEC_FIRST + 7)));
    ConnectCarriers({ObservationRecord(*m_impaired, SUBJECT_VERIFIER, EXEC_FIRST + 7)});
    ExpectBurnClosed("C1: verifier is cancelled");
}

BOOST_AUTO_TEST_CASE(finality_conflict_four_paths_and_bridge_flags)
{
    // Kind 13 FINALITY_CONFLICT (node changes section 8.5) on the real chain:
    // a digest-V2 statement of another branch signed by the lineage's current
    // committee is valid evidence in all four paths; its block sets the
    // sticky conflict, which closes burns for good (C4) in all four paths and
    // turns on bridgeFlags bit 0 (sections 7.4 and 8.5) from the first fresh
    // checkpoint at or after that block, which B3 then signs and requires.
    Prepare();
    const Consensus::Params& params{Params()};
    // Below V the wallet's deposit status is RULES_INACTIVE and
    // getbridgev2info cannot evaluate deposits.
    {
        BOOST_REQUIRE(!params.BridgeV2RulesActive(Tip()->nHeight + 1));
        const auto [status, error] = DepositStatus();
        BOOST_CHECK(status == interfaces::BridgePrevalidationResult::RULES_INACTIVE);
        BOOST_CHECK_EQUAL(error, "Bridge V2 rules are not active for the next block");
        const UniValue deposits{Cli("getbridgev2info").find_value("deposits")};
        BOOST_CHECK_EQUAL(deposits.find_value("status").get_str(), "UNAVAILABLE");
        BOOST_CHECK_EQUAL(deposits.find_value("reason").get_str(),
                          "bridge V2 rules are not active for the next block");
    }
    ProduceBlocksTo(m_G + SCALED_INTERVAL + SCALED_DEPTH);
    BOOST_REQUIRE_EQUAL(FreshFinalizedHeight(), m_G + SCALED_INTERVAL);
    // The conflict block c follows the four carrier blocks below. Place it
    // where the grid checkpoint c - depth is one CertificateDue skips
    // (G + 10k), so a delayed certificate below c can still be included
    // after c, before the next due checkpoint c + 2 >= c.
    constexpr int CARRIER_BLOCKS{4};
    while ((Tip()->nHeight + 1 + CARRIER_BLOCKS - SCALED_DEPTH - m_G) % (2 * SCALED_INTERVAL) != 0) ProduceBlock();

    // -- A READY gate: light client, deposit and three observations ---------
    BuildEthereum();
    ConnectCarriers({BootstrapRecord()});
    ConnectCarriers({UpdateRecord(EXEC_FIRST + 1)});
    ConnectCarriers({ObservationRecord(*m_healthy, SUBJECT_VERIFIER, EXEC_FIRST + 1)}, {DepositMint()});
    ConnectCarriers({ObservationRecord(*m_healthy, SUBJECT_TOKEN, EXEC_FIRST + 1),
                     ObservationRecord(*m_healthy, SUBJECT_RESERVE, EXEC_FIRST + 1)});
    BOOST_REQUIRE(!BridgeIndex().ConflictV2());
    ExpectBurnPassesGate();
    BOOST_REQUIRE(GateAt(NextBlockTime()).readiness == node::BridgeBurnReadiness::READY);
    // Deposits are open: the wallet hands out deposit instructions.
    BOOST_CHECK(DepositStatus().first == interfaces::BridgePrevalidationResult::VALID);
    BOOST_CHECK_EQUAL(Cli("getbridgev2info").find_value("deposits").find_value("status").get_str(), "OPEN");

    const int c{Tip()->nHeight + 1};
    const int delayed_checkpoint{c - SCALED_DEPTH};
    const int flagged_checkpoint{delayed_checkpoint + SCALED_INTERVAL};
    const int finalized{FreshFinalizedHeight()};
    BOOST_REQUIRE_EQUAL(finalized, delayed_checkpoint - SCALED_INTERVAL);
    BOOST_REQUIRE(flagged_checkpoint >= c);
    const uint64_t e{WithLineage([](const node::BridgeV2LineageContext& context, const node::BridgeStateIndex&) {
        return context.fresh->epoch;
    })};

    // -- Kind 13 evidence: Set'_e signs B3's epoch-e statement at the
    // finalized checkpoint with another block hash -------------------------
    const auto conflict_record{[&](const uint256& block_hash) {
        modern::FinalizedBlockV2 fb{Statement(e, finalized)};
        fb.block_hash = block_hash;
        const auto [cert, size] = CommitteeSigned(fb, e);
        BOOST_REQUIRE_EQUAL(size, 4U);
        return std::pair{fb, bridge::BridgeRecordV1{bridge::BridgeRecordKindV1::FINALITY_CONFLICT,
                                                    bridge::BridgeFinalityConflictV2{
                                                        static_cast<uint16_t>(size),
                                                        modern::EncodeCertificatePayloadV2(fb, cert)}}};
    }};
    const auto [evidence, record] = conflict_record(Keccak(std::vector<unsigned char>{'o', 't', 'h', 'e', 'r'}));
    BOOST_REQUIRE(evidence.block_hash != Statement(e, finalized).block_hash);
    // Wallet prevalidation (submitbridgecarrier) is VALID, the mempool takes
    // it, a block holding it is valid and block assembly carries it. Until a
    // block holds it, deposits stay open.
    ExpectObservationAccepted(Carrier(record));
    BOOST_CHECK(DepositStatus().first == interfaces::BridgePrevalidationResult::VALID);
    // B3's own statement re-signed is not evidence, in any path.
    {
        const modern::FinalizedBlockV2 canonical{Statement(e, finalized)};
        const auto [cert, size] = CommitteeSigned(canonical, e);
        ExpectObservationRefused(
            Carrier({bridge::BridgeRecordKindV1::FINALITY_CONFLICT,
                     bridge::BridgeFinalityConflictV2{static_cast<uint16_t>(size),
                                                      modern::EncodeCertificatePayloadV2(canonical, cert)}}),
            "bridge finality conflict equals B3's canonical statement");
    }

    // Mempool entries the conflict invalidates for good, placed straight in
    // the pool on inputs the block at c does not spend: another kind 13
    // record, a V2 burn and the burn's child, and a kind 16 vault sweep
    // (syntactic only: this chain pins no successor, but a sweep's gate is
    // the retired vault's, closed by the same row C4). ConnectTip evicts all
    // four once that block sets the sticky conflict.
    CTxMemPool& pool{*m_node.mempool};
    BOOST_REQUIRE_EQUAL(WITH_LOCK(pool.cs, return pool.size()), 0U);
    const CMpaRecord pending_mpa{BridgeMpa(conflict_record(Keccak(std::vector<unsigned char>{'p'})).second)};
    const auto pending_binding{modern::MakeBridgeBindingOutput(pending_mpa)};
    BOOST_REQUIRE(pending_binding);
    const CTransactionRef pending_conflict{MakeTransactionRef(BridgeTx(9, {*pending_binding}, pending_mpa))};
    const CTransactionRef pending_burn{MakeTransactionRef(SyntheticBurn())};
    CMutableTransaction burn_child;
    burn_child.version = 2;
    burn_child.vin = {CTxIn{COutPoint{pending_burn->GetHash(), 2}}};
    burn_child.vout = {CTxOut{pending_burn->vout[2].nValue - 10'000, CScript() << OP_TRUE}};
    const CTransactionRef pending_child{MakeTransactionRef(burn_child)};
    const CMpaRecord sweep_mpa{BridgeMpa({bridge::BridgeRecordKindV1::VAULT_SWEEP,
                                          bridge::BridgeVaultSweepV2{*modern::ConfiguredBridgeV2EntryId(params)}})};
    const auto sweep_binding{modern::MakeBridgeBindingOutput(sweep_mpa)};
    BOOST_REQUIRE(sweep_binding);
    CMutableTransaction sweep_carrier;
    sweep_carrier.version = 2;
    sweep_carrier.vin = {CTxIn{COutPoint{Txid::FromUint256(uint256{uint8_t{0x5e}}), 0}}};
    sweep_carrier.vout = {CTxOut{10'000, CScript() << OP_TRUE}, *sweep_binding};
    sweep_carrier.mpa = {sweep_mpa};
    const CTransactionRef pending_sweep{MakeTransactionRef(sweep_carrier)};
    BOOST_REQUIRE(node::HasBridgeV2FinalityConflict(*pending_conflict));
    BOOST_REQUIRE(node::HasDecentralizedBridgeBurn(*pending_burn));
    BOOST_REQUIRE(node::HasBridgeV2VaultSweep(*pending_sweep));
    BOOST_REQUIRE(!node::HasBridgeV2VaultSweep(*pending_burn));
    BOOST_REQUIRE(!node::HasDecentralizedBridgeBurn(*pending_sweep));
    for (const CTransactionRef& tx : {pending_conflict, pending_burn, pending_child, pending_sweep}) {
        TryAddToMempool(pool, TestMemPoolEntryHelper{}.Fee(10'000).FromTx(tx));
    }
    BOOST_REQUIRE_EQUAL(WITH_LOCK(pool.cs, return pool.size()), 4U);

    // -- The block at c sets the sticky conflict ----------------------------
    ConnectCarriers({record});
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, c);
    BOOST_CHECK_EQUAL(WITH_LOCK(pool.cs, return pool.size()), 0U);
    for (const CTransactionRef& tx : {pending_conflict, pending_burn, pending_child, pending_sweep}) {
        BOOST_CHECK(!WITH_LOCK(pool.cs, return pool.exists(tx->GetWitnessHash())));
    }
    const node::BridgeV2Conflict expected{bridge::BRIDGE_V2_CONFLICT_KIND13,
                                          c,
                                          Tip()->GetBlockHash(),
                                          0,
                                          modern::FinalityCertIdV2(evidence),
                                          evidence.checkpoint_index,
                                          e};
    BOOST_CHECK(BridgeIndex().ConflictV2() == std::optional<node::BridgeV2Conflict>{expected});
    const std::string kind13_closed{"sticky kind 13 conflict at height " + util::ToString(c)};
    {
        const UniValue info{Cli("getbridgev2info")};
        const UniValue& conflict{info.find_value("conflict")};
        BOOST_CHECK_EQUAL(conflict.find_value("reason").get_str(), "kind13");
        BOOST_CHECK_EQUAL(conflict.find_value("height").getInt<int>(), c);
        BOOST_CHECK_EQUAL(conflict.find_value("block").get_str(), Tip()->GetBlockHash().GetHex());
        BOOST_CHECK_EQUAL(conflict.find_value("anchor_block").getInt<uint64_t>(), 0U);
        BOOST_CHECK_EQUAL(conflict.find_value("evidence_cert_id_evm").get_str(),
                          EvmHexOf(modern::FinalityCertIdV2(evidence)));
        BOOST_CHECK_EQUAL(conflict.find_value("evidence_cert_id_evm").get_str(),
                          EvmHexOf(Keccak(evidence.Encode())));
        BOOST_CHECK_EQUAL(conflict.find_value("evidence_checkpoint_index").getInt<uint64_t>(),
                          evidence.checkpoint_index);
        BOOST_CHECK_EQUAL(conflict.find_value("evidence_epoch").getInt<uint64_t>(), e);
        const UniValue& gate_info{info.find_value("burn_gate")};
        BOOST_CHECK_EQUAL(gate_info.find_value("status").get_str(), "NOT_READY");
        BOOST_CHECK_EQUAL(gate_info.find_value("row").get_str(), "C4");
        BOOST_CHECK_EQUAL(gate_info.find_value("reason").get_str(), "sticky conflict: finality conflict evidence");
        // B3 closed deposits for good: getbridgev2info says so and the
        // wallet's deposit status refuses with the same reason.
        const UniValue& deposits{info.find_value("deposits")};
        BOOST_CHECK_EQUAL(deposits.find_value("status").get_str(), "CLOSED_FOR_GOOD");
        BOOST_CHECK_EQUAL(deposits.find_value("reason").get_str(), kind13_closed);
        const auto [status, error] = DepositStatus();
        BOOST_CHECK(status == interfaces::BridgePrevalidationResult::REJECTED);
        BOOST_CHECK_EQUAL(error, kind13_closed);
    }

    // -- Burns are refused for good in all four paths, and so is a second
    // conflict of any kind ---------------------------------------------------
    ExpectBurnClosed("C4: sticky conflict: finality conflict evidence");
    ExpectObservationRefused(Carrier(conflict_record(Keccak(std::vector<unsigned char>{'t', 'w', 'o'})).second),
                             "bridge V2 sticky conflict is already set");

    // -- bridgeFlags: 0 below c, bit 0 from c -------------------------------
    {
        const node::BridgeStateIndex& index{BridgeIndex()};
        BOOST_CHECK(node::FinalityBridgeFlagsV2(c - 1, params, &index) == std::optional<uint8_t>{0});
        BOOST_CHECK(node::FinalityBridgeFlagsV2(c, params, &index) ==
                    std::optional<uint8_t>{modern::FINALITY_BRIDGE_FLAG_DEPOSITS_CLOSED});
    }
    // The block at c + 1 carries the delayed certificate for the skipped
    // checkpoint c - depth < c: flags 0. The same statement with bit 0,
    // re-signed by the committee, is refused.
    const auto block_with = [&](const CMpaRecord& certificate) {
        const CBlock block{BuildBlock({}, certificate)};
        if (block.GetBlockTime() > GetTime()) SetMockTime(block.GetBlockTime());
        return std::pair{block, Probe(block)};
    };
    {
        const modern::FinalizedBlockV2 delayed{CheckpointStatement(delayed_checkpoint)};
        BOOST_CHECK_EQUAL(delayed.bridge_flags, 0);
        modern::FinalizedBlockV2 flagged{delayed};
        flagged.bridge_flags = modern::FINALITY_BRIDGE_FLAG_DEPOSITS_CLOSED;
        Has(block_with(CertificateRecord(flagged)).second, "fresh-bridge-flags-mismatch");
        const auto [block, probe] = block_with(CertificateRecord(delayed));
        BOOST_REQUIRE_MESSAGE(probe.empty(), probe);
        BOOST_REQUIRE(Submit(block));
        BOOST_REQUIRE_EQUAL(FreshFinalizedHeight(), delayed_checkpoint);
    }
    // The next due checkpoint is at or after c: B3's statement carries bit 0
    // (byte 184), the flags-0 variant re-signed by the committee is refused,
    // and the canonical certificate connects.
    ProduceBlocksTo(flagged_checkpoint + SCALED_DEPTH - 1);
    BOOST_REQUIRE_EQUAL(FreshFinalizedHeight(), delayed_checkpoint);
    BOOST_REQUIRE(CertificateDue().has_value());
    {
        const modern::FinalizedBlockV2 flagged{CheckpointStatement(flagged_checkpoint)};
        BOOST_CHECK_EQUAL(flagged.bridge_flags, modern::FINALITY_BRIDGE_FLAG_DEPOSITS_CLOSED);
        BOOST_CHECK_EQUAL(flagged.Encode()[184], 0x01);
        modern::FinalizedBlockV2 unflagged{flagged};
        unflagged.bridge_flags = 0;
        Has(block_with(CertificateRecord(unflagged)).second, "fresh-bridge-flags-mismatch");
        ProduceBlock(); // carries CertificateDue(): B3's flagged certificate
        BOOST_REQUIRE_EQUAL(FreshFinalizedHeight(), flagged_checkpoint);
        const auto cert_id{WithLineage([](const node::BridgeV2LineageContext& context, const node::BridgeStateIndex&) {
            return context.fresh->finalized->cert_id;
        })};
        BOOST_CHECK(cert_id == modern::FinalityCertIdV2(flagged));
        BOOST_CHECK(cert_id != modern::FinalityCertIdV2(unflagged));
    }
    ExpectBurnClosed("C4: sticky conflict: finality conflict evidence");

    // -- Replay: both trackers rebuilt from the blocks keep the conflict and
    // re-verify every certificate, the flagged one included ----------------
    {
        LOCK(cs_main);
        Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
        const CBlockIndex& tip{*chainstate.m_chain.Tip()};
        node::BridgeStateTracker& bridge{chainstate.ModernBridgeState()};
        bridge.MarkDirty();
        BOOST_REQUIRE(bridge.Sync(chainstate.m_chain, chainstate.m_blockman, params, tip));
        BOOST_CHECK(bridge.Index().ConflictV2() == std::optional<node::BridgeV2Conflict>{expected});
        node::FinalityTracker& finality{chainstate.ModernFinality()};
        finality.MarkDirty();
        BOOST_REQUIRE(finality.Sync(chainstate.m_chain, chainstate.m_blockman, params, tip, &bridge.Index()));
        BOOST_REQUIRE(finality.Current().fresh && finality.Current().fresh->finalized);
        BOOST_CHECK_EQUAL(finality.Current().fresh->finalized->height, flagged_checkpoint);
    }
    ExpectBurnClosed("C4: sticky conflict: finality conflict evidence");
    {
        const auto [status, error] = DepositStatus();
        BOOST_CHECK(status == interfaces::BridgePrevalidationResult::REJECTED);
        BOOST_CHECK_EQUAL(error, kind13_closed);
    }

    // -- B-23 retirement (node changes section 7.6): a successor pinned on
    // this chain at h_m = tip + 3. From h_m a burn naming the launch entry
    // is refused for good, so ConnectTip evicts such burns, with their
    // children, when the block at h_m - 1 connects, and not before. A burn
    // naming the successor stays. Placed straight in the pool, as above.
    BOOST_REQUIRE_EQUAL(WITH_LOCK(pool.cs, return pool.size()), 0U);
    {
        const int h_m{Tip()->nHeight + 3};
        Consensus::BridgeV2SuccessorEntry successor;
        successor.entry = V2().vault;
        successor.entry.vault_address = FillAddress(0x47);
        successor.predecessor = V2().vault.vault_address;
        successor.asset_id = *modern::ConfiguredBridgeV2AssetId(params);
        successor.migration_height = h_m;
        V2().successor = successor;
        BOOST_REQUIRE(params.BridgeV2ConfigurationValid());
        BOOST_REQUIRE(!params.BridgeV2MigrationActive(h_m - 1));
        BOOST_REQUIRE(params.BridgeV2MigrationActive(h_m));
    }
    const int h_m{V2().successor->migration_height};
    const CTransactionRef launch_burn{MakeTransactionRef(SyntheticBurn())};
    CMutableTransaction launch_burn_child;
    launch_burn_child.version = 2;
    launch_burn_child.vin = {CTxIn{COutPoint{launch_burn->GetHash(), 2}}};
    launch_burn_child.vout = {CTxOut{launch_burn->vout[2].nValue - 10'000, CScript() << OP_TRUE}};
    const CTransactionRef launch_child{MakeTransactionRef(launch_burn_child)};
    const CTransactionRef successor_burn{
        MakeTransactionRef(SyntheticBurn(Consensus::BridgeV2EntryRole::SUCCESSOR, /*fund_output=*/9))};
    BOOST_REQUIRE(!node::HasBridgeV2RetiredEntryBurn(*launch_burn, params, h_m - 1));
    BOOST_REQUIRE(node::HasBridgeV2RetiredEntryBurn(*launch_burn, params, h_m));
    BOOST_REQUIRE(!node::HasBridgeV2RetiredEntryBurn(*successor_burn, params, h_m));
    for (const CTransactionRef& tx : {launch_burn, launch_child, successor_burn}) {
        TryAddToMempool(pool, TestMemPoolEntryHelper{}.Fee(10'000).FromTx(tx));
    }
    BOOST_REQUIRE_EQUAL(WITH_LOCK(pool.cs, return pool.size()), 3U);
    ProduceBlock(); // h_m - 2: the next block is not yet h_m
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, h_m - 2);
    BOOST_CHECK_EQUAL(WITH_LOCK(pool.cs, return pool.size()), 3U);
    ProduceBlock(); // h_m - 1: the launch entry is retired for the next block
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, h_m - 1);
    BOOST_CHECK_EQUAL(WITH_LOCK(pool.cs, return pool.size()), 1U);
    BOOST_CHECK(!WITH_LOCK(pool.cs, return pool.exists(launch_burn->GetWitnessHash())));
    BOOST_CHECK(!WITH_LOCK(pool.cs, return pool.exists(launch_child->GetWitnessHash())));
    BOOST_CHECK(WITH_LOCK(pool.cs, return pool.exists(successor_burn->GetWitnessHash())));
    WITH_LOCK(pool.cs, pool.removeRecursive(*successor_burn, MemPoolRemovalReason::REORG));
    V2().successor.reset();
}

BOOST_AUTO_TEST_CASE(sponsored_sighash_in_mempool_and_blocks)
{
    // Owner ruling R10 through validation.cpp itself: mempool admission
    // (PreChecks, PolicyScriptChecks, ConsensusScriptChecks) and block
    // validation (ConnectBlock through TestBlockValidity, then a connected
    // block). A real kind 6 burn spends the minted bUSD V2, whose owner key
    // this test holds, plus a sealed-era native fee input (anyone-can-spend,
    // so only the owner's signature byte matters). Its owner may sign
    // SIGHASH_ALL|ANYONECANPAY only while the envelope's own sponsored
    // sighash pin covers the next block; plain ALL is valid either way, and
    // NONE and SINGLE (with or without ANYONECANPAY) never are.
    CKey owner_key;
    owner_key.MakeNewKey(/*fCompressed=*/true);
    FillableSigningProvider owner_provider;
    BOOST_REQUIRE(owner_provider.AddKey(owner_key));
    bridge::RecipientV1 owner;
    const CKeyID owner_id{owner_key.GetPubKey().GetID()};
    std::copy(owner_id.begin(), owner_id.end(), owner.pubkey_hash.begin());
    m_deposit_recipient = owner;

    Prepare();
    ProduceBlocksTo(m_G + SCALED_INTERVAL + SCALED_DEPTH);
    BOOST_REQUIRE_EQUAL(FreshFinalizedHeight(), m_G + SCALED_INTERVAL);
    BuildEthereum();
    BOOST_REQUIRE(m_deposit.recipient_script == GetScriptForDestination(PKHash{owner_key.GetPubKey()}));
    ConnectCarriers({BootstrapRecord()});
    ConnectCarriers({UpdateRecord(EXEC_FIRST + 1)});
    const CMutableTransaction mint{DepositMint()};
    ConnectCarriers({ObservationRecord(*m_healthy, SUBJECT_VERIFIER, EXEC_FIRST + 1)}, {mint});
    ConnectCarriers({ObservationRecord(*m_healthy, SUBJECT_TOKEN, EXEC_FIRST + 1),
                     ObservationRecord(*m_healthy, SUBJECT_RESERVE, EXEC_FIRST + 1)});
    BOOST_REQUIRE(GateAt(NextBlockTime()).readiness == node::BridgeBurnReadiness::READY);

    // The burn: [0] BURN, [1] BRIDGE_RECORD binding, [2] bUSD change to the
    // owner, [3] native change; inputs [0] the minted bUSD, [1] fund output 8.
    const Consensus::Params& params{Params()};
    const auto asset{modern::ConfiguredBridgeV2AssetId(params)};
    const auto entry{modern::ConfiguredBridgeV2EntryId(params)};
    BOOST_REQUIRE(asset && entry);
    bridge::BridgeBurnV1 burn;
    burn.registry_id = *entry;
    burn.burn_output_index = 0;
    burn.raw_amount = BURN_AMOUNT;
    burn.ethereum_recipient.fill(0x99);
    const CMpaRecord record{BridgeMpa({bridge::BridgeRecordKindV1::BRIDGE_BURN, burn})};
    const auto burned{modern::MakeAssetBurnOutput(*asset, BURN_AMOUNT)};
    const auto binding{modern::MakeBridgeBindingOutput(record)};
    const auto change{modern::MakeAssetOwnerOutput(*asset, DEPOSIT_AMOUNT - BURN_AMOUNT, m_deposit.recipient_script)};
    BOOST_REQUIRE(burned && binding && change);
    const CTxOut busd_previous{mint.vout.at(0)};
    const CTxOut fee_previous{m_fund_value / 16, CScript() << OP_TRUE};
    CMutableTransaction unsigned_burn;
    unsigned_burn.version = 2;
    unsigned_burn.vin = {CTxIn{COutPoint{mint.GetHash(), 0}}, CTxIn{COutPoint{m_fund_txid, 8}}};
    unsigned_burn.vout = {*burned, *binding, *change, CTxOut{fee_previous.nValue - CARRIER_FEE, CScript() << OP_TRUE}};
    unsigned_burn.mpa = {record};
    const auto signed_with = [&](const int hash_type) {
        CMutableTransaction spend{unsigned_burn};
        PrecomputedTransactionData txdata;
        txdata.Init(spend, std::vector<CTxOut>{busd_previous, fee_previous}, /*force=*/true);
        const MutableTransactionSignatureCreator creator{spend, 0, busd_previous.nValue, &txdata, hash_type};
        SignatureData sigdata;
        BOOST_REQUIRE(ProduceSignature(owner_provider, creator, busd_previous.scriptPubKey, sigdata,
                                       AssetSigningContext::OWNER_SUFFIX));
        UpdateInput(spend.vin[0], sigdata);
        return spend;
    };
    constexpr int ACP_ALL{SIGHASH_ALL | SIGHASH_ANYONECANPAY};
    const CMutableTransaction all{signed_with(SIGHASH_ALL)};
    const CMutableTransaction acp_all{signed_with(ACP_ALL)};
    const std::string hashtype_error{"Signature hash type missing or not understood"};
    //! Mempool admission and block validity both accept `tx`, or both
    //! refuse it on the owner's signature byte.
    const auto expect = [&](const CMutableTransaction& tx, const bool valid) {
        AlignClock();
        const std::string admitted{Admit(tx)};
        const std::string block{BlockWith(tx)};
        if (valid) {
            BOOST_CHECK_MESSAGE(admitted.empty(), admitted);
            BOOST_CHECK_MESSAGE(block.empty(), block);
        } else {
            Has(admitted, "script-verify-flag-failed");
            Has(admitted, hashtype_error);
            Has(block, "script-verify-flag-failed");
            Has(block, hashtype_error);
        }
    };

    // Without the pin (this fixture's envelope, as a reviewed mainnet one
    // would be) V2 burns are live but the burn keeps the managed-V1 rule:
    // ALL only.
    BOOST_REQUIRE(!V2().sponsored_sighash_height);
    BOOST_REQUIRE(params.BridgeV2BurnRulesActive(Tip()->nHeight + 1));
    expect(all, /*valid=*/true);
    expect(acp_all, /*valid=*/false);

    // A pin above the next block: still ALL only. A pin at the next block:
    // ALL|ANYONECANPAY passes in both paths.
    V2().sponsored_sighash_height = Tip()->nHeight + 2;
    BOOST_REQUIRE(params.BridgeV2ConfigurationValid());
    expect(acp_all, /*valid=*/false);
    V2().sponsored_sighash_height = Tip()->nHeight + 1;
    BOOST_REQUIRE(params.BridgeV2SponsoredSighashActive(Tip()->nHeight + 1));
    expect(acp_all, /*valid=*/true);
    expect(all, /*valid=*/true);
    // NONE and SINGLE leave outputs unsigned: refused under the R10 form too.
    for (const int weak : {int{SIGHASH_NONE}, int{SIGHASH_SINGLE}, int{SIGHASH_NONE | SIGHASH_ANYONECANPAY},
                           int{SIGHASH_SINGLE | SIGHASH_ANYONECANPAY}}) {
        BOOST_TEST_CONTEXT("hash type " << weak)
        {
            expect(signed_with(weak), /*valid=*/false);
        }
    }
    // Unpinned again, the approvals cached above do not carry over: the
    // selection is part of the script-cache key.
    V2().sponsored_sighash_height.reset();
    expect(acp_all, /*valid=*/false);

    // Pinned, the ALL|ANYONECANPAY burn connects and appends one leaf V2.
    V2().sponsored_sighash_height = Tip()->nHeight + 1;
    const size_t leaves_before{BridgeIndex().WithdrawalV2Count()};
    const int burn_height{Tip()->nHeight + 1};
    ProduceBlock({acp_all});
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, burn_height);
    BOOST_CHECK_EQUAL(BridgeIndex().WithdrawalV2Count(), leaves_before + 1);
    V2().sponsored_sighash_height.reset();
}

BOOST_AUTO_TEST_SUITE_END()
