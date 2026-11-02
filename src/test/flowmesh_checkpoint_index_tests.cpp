// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <chain.h>
#include <consensus/era.h>
#include <crypto/bls.h>
#include <flowmesh/bls_certificate.h>
#include <flowmesh/market.h>
#include <flowmesh/production_engine.h>
#include <modern/chain_domain.h>
#include <modern/asset_output.h>
#include <modern/flowmesh_checkpoint.h>
#include <modern/flowmesh_vault_proof.h>
#include <modern/mpa.h>
#include <node/flowmesh_checkpoint_index.h>
#include <node/flowmesh_vault_index.h>
#include <node/fn_seat_index.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr int H{100};
constexpr int A1{120};
constexpr int A2{130};
constexpr int A3{A2 + Consensus::FLOWMESH_ANCHOR_DEPTH};
constexpr int C0{A3 + Consensus::FLOWMESH_ANCHOR_DEPTH + 1};

uint256 Filled(const unsigned char value)
{
    uint256 out;
    std::fill(out.begin(), out.end(), value);
    return out;
}

Consensus::Params Params()
{
    Consensus::Params params{};
    params.legacy_b3coin = true;
    params.hard_fork_height = H + 1;
    params.transition_pow_length = 10;
    params.hashGenesisBlock = Filled(0x01);
    params.legacy_final_hash = Filled(0x02);
    params.modern_pos.emplace();
    params.modern_pos->treasury_script = {
        static_cast<unsigned char>(OP_TRUE)};
    params.fn_genesis_rights_root = Filled(0x03);
    Consensus::FnGenesisRight right;
    right.pod_id = Filled(0x04);
    right.recipient_key_hash.fill(0x05);
    params.fn_genesis_manifest.push_back(right);
    params.fn_pod_activation_height = A1;
    params.asset_activation_height = A2;
    params.flowmesh_activation_height = A3;
    return params;
}

bls::SecretKey Key(const uint32_t index)
{
    std::array<unsigned char, 32> ikm{};
    ikm[0] = static_cast<unsigned char>(index + 1);
    ikm[15] = static_cast<unsigned char>(index * 19 + 7);
    ikm[31] = 0x91;
    const auto key{bls::SecretKey::FromIKM(ikm)};
    BOOST_REQUIRE(key.has_value());
    return *key;
}

struct SyntheticChain {
    std::vector<uint256> hashes;
    std::vector<CBlockIndex> indexes;
    CChain chain;

    explicit SyntheticChain(const int tip_height)
        : hashes(static_cast<size_t>(tip_height + 1)),
          indexes(static_cast<size_t>(tip_height + 1))
    {
        for (int height{0}; height <= tip_height; ++height) {
            hashes[height] = Filled(static_cast<unsigned char>(height + 1));
            indexes[height].nHeight = height;
            indexes[height].phashBlock = &hashes[height];
            indexes[height].pprev =
                height == 0 ? nullptr : &indexes[height - 1];
        }
        chain.SetTip(indexes.back());
    }
};

class TestAnchorPolicy final : public flowmesh::AnchorPolicy
{
public:
    explicit TestAnchorPolicy(const CChain& chain) : m_chain{chain} {}

    bool Acceptable(const flowmesh::AnchorRef& anchor) const override
    {
        return StillCanonical(anchor);
    }

    bool StillCanonical(const flowmesh::AnchorRef& anchor) const override
    {
        if (anchor.height < 0) return false;
        const CBlockIndex* index{m_chain[anchor.height]};
        return index != nullptr && index->GetBlockHash() == anchor.hash;
    }

    flowmesh::AnchorRef Current() const override
    {
        const CBlockIndex* tip{m_chain.Tip()};
        return tip == nullptr
                   ? flowmesh::AnchorRef{}
                   : flowmesh::AnchorRef{tip->nHeight, tip->GetBlockHash()};
    }

private:
    const CChain& m_chain;
};

struct Fixture {
    Consensus::Params params{Params()};
    uint256 domain;
    modern::AssetId asset{Filled(0x31)};
    flowmesh::MarketId market;
    flowmesh::VaultId vault;
    SyntheticChain chain{C0 + 12};
    node::FnSeatIndex seats;
    node::FlowMeshVaultIndex vaults;
    std::vector<bls::SecretKey> secrets;

    explicit Fixture(
        const std::optional<int> second_rotation_height = std::nullopt)
    {
        const auto d{modern::ModernChainDomain(params.hashGenesisBlock,
                                                *params.legacy_final_hash)};
        BOOST_REQUIRE(d.has_value());
        domain = *d;
        const auto m{flowmesh::ComputeFlowMeshMarketId(domain, asset)};
        BOOST_REQUIRE(m.has_value());
        market = *m;
        const auto v{flowmesh::ComputeFlowMeshVaultId(domain, market)};
        BOOST_REQUIRE(v.has_value());
        vault = *v;

        const uint32_t key_count{second_rotation_height ? 6U : 5U};
        for (uint32_t i{0}; i < key_count; ++i) secrets.push_back(Key(i));

        node::FnSeatBlockDelta first;
        first.height = A2;
        first.block_hash = chain.hashes[A2];
        for (uint32_t i{0}; i < 4; ++i) first.added.push_back(Seat(i, A2));
        ConnectSeat(first);

        for (int height{A2 + 1}; height <= A3; ++height) {
            node::FnSeatBlockDelta empty;
            empty.height = height;
            empty.block_hash = chain.hashes[height];
            ConnectSeat(empty);
        }

        // One key rotates after market genesis. The A3 and A3+1 anchors thus
        // provide exact outgoing and incoming epoch snapshots.
        node::FnSeatBlockDelta rotate;
        rotate.height = A3 + 1;
        rotate.block_hash = chain.hashes[A3 + 1];
        rotate.removed.push_back(first.added[0]);
        rotate.added.push_back(Seat(4, A3 + 1));
        ConnectSeat(rotate);
        for (int height{A3 + 2}; height <= chain.chain.Height(); ++height) {
            node::FnSeatBlockDelta delta;
            delta.height = height;
            delta.block_hash = chain.hashes[height];
            if (second_rotation_height &&
                height == *second_rotation_height) {
                delta.removed.push_back(first.added[1]);
                delta.added.push_back(Seat(5, height));
            }
            ConnectSeat(delta);
        }

        const flowmesh::AccountId account{Filled(0x32)};
        const COutPoint establishing{
            Txid::FromUint256(Filled(0x33)), 0};
        node::FlowMeshVaultRecord deposit{
            establishing,
            asset,
            1,
            vault,
            modern::VAULT_KIND_USER_DEPOSIT,
            modern::FlowMeshUserDepositShard(vault, account),
            account,
            A3,
            chain.hashes[A3]};
        node::FlowMeshMarketRecord market_record{
            asset, market, vault, establishing, A3, chain.hashes[A3]};
        node::FlowMeshVaultBlockDelta genesis;
        genesis.height = A3;
        genesis.block_hash = chain.hashes[A3];
        genesis.added.push_back(deposit);
        genesis.markets_added.push_back(market_record);
        ConnectVault(genesis);
        for (int height{A3 + 1}; height <= chain.chain.Height(); ++height) {
            node::FlowMeshVaultBlockDelta empty;
            empty.height = height;
            empty.block_hash = chain.hashes[height];
            ConnectVault(empty);
        }
    }

    node::FnSeatRecord Seat(const uint32_t key_index,
                            const int created_height) const
    {
        node::FnSeatRecord out;
        out.outpoint = COutPoint{
            Txid::FromUint256(Filled(static_cast<unsigned char>(0x50 + key_index))),
            key_index};
        out.seat_id = flowmesh::ComputeFlowMeshSeatId(domain, out.outpoint);
        out.bls_pubkey = secrets[key_index].GetPublicKey().Compressed();
        out.proof_of_possession = secrets[key_index].SignPoP().Compressed();
        out.created_height = created_height;
        out.created_block = chain.hashes[created_height];
        return out;
    }

    void ConnectSeat(const node::FnSeatBlockDelta& delta)
    {
        std::string error;
        BOOST_REQUIRE_MESSAGE(seats.ConnectBlock(delta, error), error);
    }

    void ConnectVault(const node::FlowMeshVaultBlockDelta& delta)
    {
        std::string error;
        BOOST_REQUIRE_MESSAGE(vaults.ConnectBlock(delta, error), error);
    }

    //! `anchor_hash`, if given, replaces the canonical hash at
    //! `anchor_height` (the seats are still those of the canonical block).
    flowmesh::ActiveFnBlsSeatSet Active(
        const int anchor_height, const uint64_t epoch,
        const std::optional<uint256>& anchor_hash = std::nullopt) const
    {
        const auto snapshot{seats.SnapshotAt(chain.indexes[anchor_height])};
        BOOST_REQUIRE(snapshot.has_value());
        std::vector<flowmesh::BlsSeatBinding> bindings;
        for (const node::FnSeatRecord& member : snapshot->members) {
            bindings.push_back({member.outpoint, member.bls_pubkey,
                                member.proof_of_possession});
        }
        flowmesh::BlsSeatSetCheck check{flowmesh::BlsSeatSetCheck::BAD_SET_HASH};
        const auto active{flowmesh::BuildActiveFnBlsSeatSet(
            domain, market, epoch, anchor_height,
            anchor_hash.value_or(chain.hashes[anchor_height]), bindings,
            check)};
        BOOST_REQUIRE_MESSAGE(active.has_value(),
                              flowmesh::BlsSeatSetCheckName(check));
        return *active;
    }

    modern::FlowMeshCheckpointRecordV1 Certify(
        modern::FlowMeshCheckpointCoreV1 core) const
    {
        const auto active{Active(static_cast<int>(core.anchor.height),
                                 core.epoch)};
        BOOST_REQUIRE(core.seat_set_hash == active.set_hash);
        const auto context{modern::FlowMeshCheckpointBlsContextV1(core)};
        std::vector<flowmesh::IndexedBlsSignature> partials;
        for (uint32_t i{0}; i < flowmesh::FlowMeshBlsThreshold(active.Size());
             ++i) {
            const auto key_bytes{active.members[i].key.Key().Compressed()};
            const auto secret{std::find_if(
                secrets.begin(), secrets.end(), [&](const bls::SecretKey& key) {
                    return key.GetPublicKey().Compressed() == key_bytes;
                })};
            BOOST_REQUIRE(secret != secrets.end());
            const auto signature{flowmesh::SignBlsMicroblockCertificate(
                *secret, context, active)};
            BOOST_REQUIRE(signature.has_value());
            partials.push_back({i, *signature});
        }
        flowmesh::BlsCertificateAssemblyCheck check{
            flowmesh::BlsCertificateAssemblyCheck::AGGREGATION_FAILED};
        const auto certificate{flowmesh::AssembleBlsMicroblockCertificate(
            context, active, partials, check)};
        BOOST_REQUIRE(certificate.has_value());
        return modern::FlowMeshCheckpointRecordV1{core, *certificate};
    }

    CMpaRecord CheckpointRecord(modern::FlowMeshCheckpointCoreV1 core) const
    {
        const auto active{Active(static_cast<int>(core.anchor.height),
                                 core.epoch)};
        core.seat_set_hash = active.set_hash;
        BindProductionIdentity(core);
        const auto bytes{modern::EncodeFlowMeshCheckpointRecordV1(
            Certify(core), active.Size())};
        BOOST_REQUIRE(bytes.has_value());
        return CMpaRecord{modern::MPA_TYPE_FLOWMESH_CHECKPOINT,
                          modern::MPA_VERSION_V1, *bytes};
    }

    modern::FlowMeshDepositAcceptanceV1 Deposit(const uint64_t sequence) const
    {
        modern::FlowMeshDepositAcceptanceV1 effect;
        effect.acceptance_id = Filled(0x71);
        effect.market_id = market;
        effect.epoch = 0;
        effect.sequence = sequence;
        effect.deposit_outpoint =
            COutPoint{Txid::FromUint256(Filled(0x72)), 3};
        effect.account = Filled(0x73);
        effect.asset = asset;
        effect.amount = 90;
        effect.vault_id = vault;
        effect.shard = modern::FlowMeshUserDepositShard(vault, effect.account);
        return effect;
    }

    modern::FlowMeshCheckpointCoreV1 Core(
        const int anchor_height, const uint64_t epoch, const uint64_t sequence,
        const modern::FlowMeshCheckpointId& previous,
        const std::span<const modern::FlowMeshEffectV1> effects = {},
        const uint64_t effect_start = 0) const
    {
        modern::FlowMeshCheckpointCoreV1 core;
        core.domain = domain;
        core.market_id = market;
        core.epoch = epoch;
        core.sequence = sequence;
        core.previous_checkpoint_id = previous;
        core.anchor = {static_cast<uint64_t>(anchor_height),
                       chain.hashes[anchor_height]};
        core.seat_set_hash = Active(anchor_height, epoch).set_hash;
        core.production_anchor = core.anchor;
        if (sequence != 0) {
            core.parent_hash =
                Filled(static_cast<unsigned char>(0x80 + sequence));
        }
        core.previous_state_root =
            Filled(static_cast<unsigned char>(0xa0 + sequence));
        core.actions_root =
            Filled(static_cast<unsigned char>(0xb0 + sequence));
        core.result_root =
            Filled(static_cast<unsigned char>(0xc0 + sequence));
        core.state_root = Filled(static_cast<unsigned char>(0x90 + sequence));
        core.effect_start = effect_start;
        core.effect_count = static_cast<uint32_t>(effects.size());
        const auto root{modern::ComputeFlowMeshEffectRoot(effect_start, effects)};
        BOOST_REQUIRE(root.has_value());
        core.effect_root = *root;
        BindProductionIdentity(core);
        return core;
    }

    //! The empty genesis on the seats at `anchor_height`. Consensus accepts
    //! only the defaults: epoch zero, produced at the seat anchor itself.
    modern::FlowMeshCheckpointCoreV1 Genesis(
        const int anchor_height, const uint64_t epoch = 0,
        const std::optional<int> production_height = std::nullopt) const
    {
        const auto active{Active(anchor_height, epoch)};
        const flowmesh::FlowMeshState initial_state{
            vault, asset, modern::NativeAsset(),
            flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
        const flowmesh::ProductionEpochGate gate{domain, market, active};
        const TestAnchorPolicy policy{chain.chain};
        const int produced_at{production_height.value_or(anchor_height)};
        const flowmesh::AnchorRef anchor{produced_at,
                                          chain.hashes[produced_at]};
        const flowmesh::ProductionAnchorContext context{
            chain.chain.Height(), std::nullopt, &policy};
        const CScript treasury_script{
            params.modern_pos->treasury_script.begin(),
            params.modern_pos->treasury_script.end()};
        const uint256 treasury{
            modern::AssetOwnerCommitment(treasury_script)};
        const std::vector<flowmesh::Action> no_actions;
        flowmesh::ProductionEntryCheck check;
        const auto built{flowmesh::BuildProductionExecutionEntry(
            initial_state, domain, market, active, gate, /*sequence=*/0,
            /*effect_start=*/0, uint256{}, anchor, context, treasury,
            no_actions, /*deposits=*/nullptr, check)};
        BOOST_REQUIRE_MESSAGE(built.has_value(),
                              flowmesh::ProductionEntryCheckName(check));

        modern::FlowMeshCheckpointCoreV1 core;
        core.domain = domain;
        core.market_id = market;
        core.epoch = epoch;
        core.sequence = 0;
        core.microblock_hash = built->entry.GetHash();
        core.anchor = {static_cast<uint64_t>(anchor_height),
                       chain.hashes[anchor_height]};
        core.seat_set_hash = active.set_hash;
        core.production_anchor = {static_cast<uint64_t>(produced_at),
                                  anchor.hash};
        core.previous_state_root = built->entry.previous_state_root;
        core.actions_root = built->entry.actions_root;
        core.result_root = built->entry.result_root;
        core.state_root = built->entry.state_root;
        core.effect_start = built->entry.effect_start;
        core.effect_count = built->entry.effect_count;
        core.effect_root = built->entry.effect_root;
        BOOST_REQUIRE(modern::IsCanonicalFlowMeshCheckpointCoreV1(core));
        return core;
    }

    static void BindProductionIdentity(
        modern::FlowMeshCheckpointCoreV1& core)
    {
        const auto identity{
            modern::FlowMeshCheckpointProductionIdentityV1(core)};
        BOOST_REQUIRE(identity.has_value());
        core.microblock_hash = *identity;
    }
};

CTransactionRef Tx(std::vector<CMpaRecord> records, const uint32_t salt = 1)
{
    CMutableTransaction tx;
    tx.version = 2;
    tx.nLockTime = salt;
    tx.mpa = std::move(records);
    return MakeTransactionRef(std::move(tx));
}

CBlock Block(std::initializer_list<CTransactionRef> txs)
{
    CBlock block;
    block.vtx.assign(txs.begin(), txs.end());
    return block;
}

CScript OwnerScript(const unsigned char value)
{
    return CScript() << OP_DUP << OP_HASH160
                     << std::vector<unsigned char>(20, value)
                     << OP_EQUALVERIFY << OP_CHECKSIG;
}

CMpaRecord ProofRecord(const modern::FlowMeshVaultProofV1& proof)
{
    const auto bytes{modern::EncodeFlowMeshVaultProofV1(proof)};
    BOOST_REQUIRE(bytes.has_value());
    return CMpaRecord{modern::MPA_TYPE_FLOWMESH_VAULT_PROOF,
                      modern::MPA_VERSION_V1, *bytes};
}

//! A certified checkpoint record with `change` applied to its core after
//! signing. The production identity, and the certificate's copy of it, are
//! rebound and the signature kept: it still verifies only if the change
//! leaves the identity as it was.
template <typename Change>
CMpaRecord Altered(const CMpaRecord& record, const size_t seat_count,
                   const Change& change)
{
    auto decoded{
        modern::DecodeFlowMeshCheckpointRecordV1(record.payload, seat_count)};
    BOOST_REQUIRE(decoded.has_value());
    change(decoded->core);
    Fixture::BindProductionIdentity(decoded->core);
    decoded->certificate.microblock_hash = decoded->core.microblock_hash;
    const auto bytes{
        modern::EncodeFlowMeshCheckpointRecordV1(*decoded, seat_count)};
    BOOST_REQUIRE(bytes.has_value());
    return CMpaRecord{modern::MPA_TYPE_FLOWMESH_CHECKPOINT,
                      modern::MPA_VERSION_V1, *bytes};
}

//! A certified checkpoint record naming another previous checkpoint. Neither
//! the certificate nor the production identity covers that field, so the
//! record stays validly certified: no key is needed to do this.
CMpaRecord Relabelled(const CMpaRecord& record, const size_t seat_count,
                      const modern::FlowMeshCheckpointId& previous)
{
    return Altered(record, seat_count,
                   [&](modern::FlowMeshCheckpointCoreV1& core) {
                       core.previous_checkpoint_id = previous;
                   });
}

node::FlowMeshCheckpointBlockDelta Verify(
    const node::FlowMeshCheckpointIndex& index, const Fixture& fixture,
    const CBlock& block, const int height, const unsigned char block_fill)
{
    node::FlowMeshCheckpointBlockDelta delta;
    std::string error;
    BOOST_REQUIRE_MESSAGE(index.VerifyBlock(
                              block, height, Filled(block_fill),
                              fixture.chain.chain, fixture.params,
                              fixture.seats, fixture.vaults, delta, error),
                          error);
    return delta;
}

void Connect(node::FlowMeshCheckpointIndex& index,
             const node::FlowMeshCheckpointBlockDelta& delta)
{
    std::string error;
    BOOST_REQUIRE_MESSAGE(index.ConnectBlock(delta, error), error);
}

uint256 NumberedId(const unsigned char domain, const uint32_t number)
{
    uint256 out;
    out.begin()[0] = static_cast<unsigned char>(number);
    out.begin()[1] = static_cast<unsigned char>(number >> 8);
    out.begin()[2] = static_cast<unsigned char>(number >> 16);
    out.begin()[3] = static_cast<unsigned char>(number >> 24);
    out.begin()[31] = domain;
    return out;
}

flowmesh::WithdrawalSettlementFactV1 SettlementFact(
    const Fixture& fixture,
    const modern::FlowMeshCheckpointId& checkpoint_id,
    const int height, const uint32_t number)
{
    flowmesh::WithdrawalSettlementFactV1 fact;
    fact.receipt.receipt_id = NumberedId(0xe1, number + 1);
    fact.receipt.market_id = fixture.market;
    fact.receipt.epoch = 0;
    fact.receipt.sequence = number;
    fact.receipt.account = NumberedId(0xe2, number + 1);
    fact.receipt.asset = fixture.asset;
    fact.receipt.amount = 1;
    fact.receipt.destination_owner_commitment = NumberedId(0xe3, number + 1);
    fact.receipt.vault_id = fixture.vault;
    fact.receipt.deterministic_change_shard =
        static_cast<uint16_t>(number % 256);
    fact.checkpoint_id = checkpoint_id;
    fact.transaction_id = Txid::FromUint256(NumberedId(0xe4, number + 1));
    fact.connected_height = height;
    fact.connected_block = fixture.chain.hashes[height];
    return fact;
}

void AppendSettlementBlock(
    node::FlowMeshCheckpointIndex& index, const Fixture& fixture,
    const modern::FlowMeshCheckpointId& checkpoint_id, const int height,
    const uint32_t first_number, const size_t count)
{
    node::FlowMeshCheckpointBlockDelta delta;
    delta.height = height;
    delta.block_hash = fixture.chain.hashes[height];
    delta.withdrawal_settlements.reserve(count);
    delta.nullifiers.reserve(count);
    for (size_t i{0}; i < count; ++i) {
        auto fact{SettlementFact(
            fixture, checkpoint_id, height,
            first_number + static_cast<uint32_t>(i))};
        delta.nullifiers.push_back(
            {node::FlowMeshNullifierKind::WITHDRAWAL_RECEIPT,
             fact.receipt.receipt_id});
        delta.withdrawal_settlements.push_back(std::move(fact));
    }
    Connect(index, delta);
}

} // namespace

BOOST_AUTO_TEST_SUITE(flowmesh_checkpoint_index_tests)

BOOST_AUTO_TEST_CASE(checkpoint_heads_ranges_bls_and_exact_undo)
{
    const Fixture fixture;
    node::FlowMeshCheckpointIndex index;
    const modern::FlowMeshEffectV1 deposit{fixture.Deposit(1)};
    const std::vector<modern::FlowMeshEffectV1> effects{deposit};
    const auto first_core{fixture.Genesis(A3)};
    const auto first_id{modern::FlowMeshCheckpointIdV1(first_core)};
    BOOST_REQUIRE(first_id.has_value());
    const auto second_core{fixture.Core(A3, 0, 1, *first_id, effects, 0)};
    const auto second_id{modern::FlowMeshCheckpointIdV1(second_core)};
    BOOST_REQUIRE(second_id.has_value());

    // Two checkpoint records advance one market in exact MPA order.
    const CBlock block{Block({Tx({fixture.CheckpointRecord(first_core),
                                  fixture.CheckpointRecord(second_core)})})};
    const auto delta{Verify(index, fixture, block, C0, 0xa1)};
    BOOST_REQUIRE_EQUAL(delta.checkpoints.size(), 2U);
    Connect(index, delta);
    BOOST_REQUIRE(index.Head(fixture.market).has_value());
    BOOST_CHECK(index.Head(fixture.market)->checkpoint_id == *second_id);
    BOOST_CHECK_EQUAL(index.CheckpointCount(), 2U);

    std::string error;
    BOOST_CHECK(!index.DisconnectBlock(C0, Filled(0xa2), error));
    BOOST_CHECK_EQUAL(index.CheckpointCount(), 2U);
    BOOST_REQUIRE(index.DisconnectBlock(C0, Filled(0xa1), error));
    BOOST_CHECK(!index.Head(fixture.market));
    BOOST_CHECK_EQUAL(index.CheckpointCount(), 0U);

    // A stale head, a skipped effect position, and a bad aggregate signature
    // are each rejected without touching state.
    Connect(index, Verify(index, fixture,
                          Block({Tx({fixture.CheckpointRecord(first_core)})}),
                          C0, 0xa3));
    auto stale{fixture.Core(A3, 0, 3, {}, {}, 1)};
    node::FlowMeshCheckpointBlockDelta ignored;
    BOOST_CHECK(!index.VerifyBlock(
        Block({Tx({fixture.CheckpointRecord(stale)})}), C0,
        Filled(0xa4), fixture.chain.chain, fixture.params, fixture.seats,
        fixture.vaults, ignored, error));
    BOOST_CHECK_EQUAL(error,
                      "FlowMesh checkpoint does not extend the current market head");

    auto skipped{fixture.Core(A3, 0, 3, *first_id, {}, 2)};
    BOOST_CHECK(!index.VerifyBlock(
        Block({Tx({fixture.CheckpointRecord(skipped)})}), C0,
        Filled(0xa5), fixture.chain.chain, fixture.params, fixture.seats,
        fixture.vaults, ignored, error));
    BOOST_CHECK_EQUAL(error,
                      "FlowMesh checkpoint effect range is not consecutive");

    auto bad_record{fixture.CheckpointRecord(second_core)};
    bad_record.payload.back() ^= 1;
    BOOST_CHECK(!index.VerifyBlock(Block({Tx({bad_record})}), C0,
                                   Filled(0xa6), fixture.chain.chain,
                                   fixture.params, fixture.seats,
                                   fixture.vaults, ignored, error));
    BOOST_CHECK(error.find("certificate is invalid") != std::string::npos);

    // Nor may a range start inside the head's: its effects would take
    // positions the head's deposit already holds.
    Connect(index, Verify(index, fixture,
                          Block({Tx({fixture.CheckpointRecord(second_core)})}),
                          C0 + 1, 0xac));
    const std::vector<modern::FlowMeshEffectV1> later{fixture.Deposit(2)};
    const auto overlapping{fixture.Core(A3, 0, 2, *second_id, later, 0)};
    BOOST_CHECK(!index.VerifyBlock(
        Block({Tx({fixture.CheckpointRecord(overlapping)})}), C0 + 2,
        Filled(0xad), fixture.chain.chain, fixture.params, fixture.seats,
        fixture.vaults, ignored, error));
    BOOST_CHECK_EQUAL(error,
                      "FlowMesh checkpoint effect range is not consecutive");
}

BOOST_AUTO_TEST_CASE(settlement_scan_accepts_exact_pre_activation_boundary)
{
    const Fixture fixture;
    node::FlowMeshCheckpointIndex index;
    std::string error;
    for (int height{A3}; height <= A3 + 2; ++height) {
        node::FlowMeshCheckpointBlockDelta delta;
        delta.height = height;
        delta.block_hash = fixture.chain.hashes[height];
        Connect(index, delta);
    }

    std::vector<flowmesh::WithdrawalSettlementFactV1> settlements;
    BOOST_REQUIRE_MESSAGE(index.WithdrawalSettlementsBetween(
                              fixture.market, fixture.chain.indexes[A3 - 1],
                              fixture.chain.indexes[A3 + 2], settlements,
                              error),
                          error);
    BOOST_CHECK(settlements.empty());

    // Only the exact predecessor may stand outside retained history; a wider
    // gap could hide records and therefore remains fail-closed.
    BOOST_CHECK(!index.WithdrawalSettlementsBetween(
        fixture.market, fixture.chain.indexes[A3 - 2],
        fixture.chain.indexes[A3 + 2], settlements, error));

    int selected_height{-1};
    size_t selected_count{0};
    BOOST_REQUIRE_MESSAGE(index.WithdrawalSettlementCatchupHeight(
                              fixture.market, fixture.chain.indexes[A3 - 1],
                              fixture.chain.indexes[A3 + 2], 1,
                              selected_height, selected_count, error), error);
    BOOST_CHECK_EQUAL(selected_height, A3 + 2);
    BOOST_CHECK_EQUAL(selected_count, 0U);
    BOOST_CHECK(!index.WithdrawalSettlementCatchupHeight(
        fixture.market, fixture.chain.indexes[A3 - 2],
        fixture.chain.indexes[A3 + 2], 1, selected_height,
        selected_count, error));
}

BOOST_AUTO_TEST_CASE(settlement_short_tail_work_is_independent_of_retained_prefix)
{
    constexpr int FIRST{100};
    constexpr int BLOCKS{300'000};
    constexpr int LAST{FIRST + BLOCKS - 1};
    constexpr int INTERVAL{3};
    SyntheticChain chain{LAST};
    for (int height{0}; height <= LAST; ++height) {
        chain.hashes[height] = NumberedId(0xe8, height);
        chain.indexes[height].BuildSkip();
    }
    node::FlowMeshCheckpointIndex index;
    std::string error;
    for (int height{FIRST}; height <= LAST; ++height) {
        node::FlowMeshCheckpointBlockDelta delta;
        delta.height = height;
        delta.block_hash = chain.hashes[height];
        BOOST_REQUIRE_MESSAGE(index.ConnectBlock(delta, error), error);
    }
    const auto market{Filled(0xe5)};
    std::vector<flowmesh::WithdrawalSettlementFactV1> settlements;
    node::FlowMeshSettlementScanWork work;
    const auto& after{chain.indexes[LAST - INTERVAL]};
    const auto& through{chain.indexes[LAST]};
    BOOST_REQUIRE_MESSAGE(index.WithdrawalSettlementsBetween(
                              market, after, through, settlements, error,
                              &work), error);
    BOOST_CHECK(settlements.empty());
    BOOST_TEST_MESSAGE("settlement tail: anchors=" << work.anchor_records
                       << ", range=" << work.range_records);
    BOOST_CHECK_EQUAL(work.anchor_records, 2U);
    BOOST_CHECK_EQUAL(work.range_records, INTERVAL);

    int selected_height{-1};
    size_t selected_count{0};
    BOOST_REQUIRE_MESSAGE(index.WithdrawalSettlementCatchupHeight(
                              market, after, through, 1, selected_height,
                              selected_count, error, &work), error);
    BOOST_CHECK_EQUAL(selected_height, LAST);
    BOOST_CHECK_EQUAL(selected_count, 0U);
    BOOST_TEST_MESSAGE("catch-up tail: anchors=" << work.anchor_records
                       << ", range=" << work.range_records);
    BOOST_CHECK_EQUAL(work.anchor_records, 2U);
    BOOST_CHECK_EQUAL(work.range_records, INTERVAL);

    // A bounded microbenchmark is descriptive only. Work counts above, not
    // scheduling-sensitive wall time, are this regression's assertions.
    constexpr size_t REPEATS{100};
    const auto started{std::chrono::steady_clock::now()};
    for (size_t repeat{0}; repeat < REPEATS; ++repeat) {
        BOOST_REQUIRE(index.WithdrawalSettlementsBetween(
            market, after, through, settlements, error));
        BOOST_REQUIRE(index.WithdrawalSettlementCatchupHeight(
            market, after, through, 1, selected_height, selected_count, error));
    }
    const auto elapsed{std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count()};
    BOOST_TEST_MESSAGE("300000 retained / 3-block tail / 100 query pairs: "
                       << elapsed << " us (not a performance threshold)");

    // Exact endpoint identity and reorg handling still apply at a large
    // offset, and Disconnect/Connect must not leave stale offset state.
    BOOST_REQUIRE(index.DisconnectBlock(LAST, chain.hashes[LAST], error));
    BOOST_CHECK(!index.WithdrawalSettlementsBetween(
        market, after, through, settlements, error));
    node::FlowMeshCheckpointBlockDelta replacement;
    replacement.height = LAST;
    replacement.block_hash = NumberedId(0xe6, LAST);
    BOOST_REQUIRE(index.ConnectBlock(replacement, error));
    BOOST_CHECK(!index.WithdrawalSettlementCatchupHeight(
        market, after, through, 1, selected_height, selected_count, error));
    chain.hashes[LAST] = replacement.block_hash;
    BOOST_REQUIRE(index.WithdrawalSettlementsBetween(
        market, after, through, settlements, error, &work));
    BOOST_CHECK_EQUAL(work.anchor_records, 2U);
    BOOST_CHECK_EQUAL(work.range_records, INTERVAL);

    index.Clear();
    BOOST_CHECK(!index.WithdrawalSettlementsBetween(
        market, after, through, settlements, error));
    BOOST_CHECK(!index.WithdrawalSettlementCatchupHeight(
        market, after, through, 1, selected_height, selected_count, error));
}

BOOST_AUTO_TEST_CASE(settlement_range_checks_every_interior_canonical_block)
{
    constexpr int FIRST{100};
    constexpr int LAST{FIRST + 5};
    SyntheticChain chain{LAST};
    node::FlowMeshCheckpointIndex index;
    std::string error;
    for (int height{FIRST}; height <= LAST; ++height) {
        node::FlowMeshCheckpointBlockDelta delta;
        delta.height = height;
        delta.block_hash = chain.hashes[height];
        BOOST_REQUIRE_MESSAGE(index.ConnectBlock(delta, error), error);
    }
    // Endpoints still match, but one interior canonical identity differs.
    // Empty blocks cannot be skipped merely because they hold no receipts.
    chain.hashes[FIRST + 2] = Filled(0xe7);
    const auto market{Filled(0xe5)};
    std::vector<flowmesh::WithdrawalSettlementFactV1> settlements;
    BOOST_CHECK(!index.WithdrawalSettlementsBetween(
        market, chain.indexes[FIRST], chain.indexes[LAST], settlements, error));
    BOOST_CHECK(settlements.empty());
    BOOST_CHECK_EQUAL(error, "FlowMesh settlement interval does not match indexed history");
    int selected_height{-1};
    size_t selected_count{0};
    BOOST_CHECK(!index.WithdrawalSettlementCatchupHeight(
        market, chain.indexes[FIRST], chain.indexes[LAST], 1,
        selected_height, selected_count, error));
    BOOST_CHECK_EQUAL(error, "FlowMesh settlement catch-up interval does not match indexed history");
}

BOOST_AUTO_TEST_CASE(offline_settlement_backlog_uses_bounded_block_anchors)
{
    const Fixture fixture;
    node::FlowMeshCheckpointIndex index;
    const auto core{fixture.Genesis(A3)};
    const auto checkpoint_id{modern::FlowMeshCheckpointIdV1(core)};
    BOOST_REQUIRE(checkpoint_id.has_value());
    Connect(index, Verify(index, fixture,
                          Block({Tx({fixture.CheckpointRecord(core)})}),
                          C0, static_cast<unsigned char>(C0 + 1)));

    constexpr size_t LIMIT{
        flowmesh::FLOWMESH_MAX_WITHDRAWAL_SETTLEMENTS_PER_ENTRY};
    AppendSettlementBlock(index, fixture, *checkpoint_id, C0 + 1,
                          /*first_number=*/0, LIMIT);
    AppendSettlementBlock(index, fixture, *checkpoint_id, C0 + 2,
                          static_cast<uint32_t>(LIMIT), 1);

    int selected_height{-1};
    size_t selected_count{0};
    std::string error;
    BOOST_REQUIRE_MESSAGE(index.WithdrawalSettlementCatchupHeight(
                              fixture.market, fixture.chain.indexes[C0],
                              fixture.chain.indexes[C0 + 2], LIMIT,
                              selected_height, selected_count, error),
                          error);
    BOOST_CHECK_EQUAL(selected_height, C0 + 1);
    BOOST_CHECK_EQUAL(selected_count, LIMIT);

    std::vector<flowmesh::WithdrawalSettlementFactV1> settlements;
    BOOST_REQUIRE_MESSAGE(index.WithdrawalSettlementsBetween(
                              fixture.market, fixture.chain.indexes[C0],
                              fixture.chain.indexes[selected_height],
                              settlements, error),
                          error);
    BOOST_CHECK_EQUAL(settlements.size(), LIMIT);

    BOOST_REQUIRE_MESSAGE(index.WithdrawalSettlementCatchupHeight(
                              fixture.market,
                              fixture.chain.indexes[selected_height],
                              fixture.chain.indexes[C0 + 2], LIMIT,
                              selected_height, selected_count, error),
                          error);
    BOOST_CHECK_EQUAL(selected_height, C0 + 2);
    BOOST_CHECK_EQUAL(selected_count, 1U);
}

BOOST_AUTO_TEST_CASE(one_block_settlement_overflow_fails_closed)
{
    const Fixture fixture;
    node::FlowMeshCheckpointIndex index;
    const auto core{fixture.Genesis(A3)};
    const auto checkpoint_id{modern::FlowMeshCheckpointIdV1(core)};
    BOOST_REQUIRE(checkpoint_id.has_value());
    Connect(index, Verify(index, fixture,
                          Block({Tx({fixture.CheckpointRecord(core)})}),
                          C0, static_cast<unsigned char>(C0 + 1)));

    constexpr size_t LIMIT{
        flowmesh::FLOWMESH_MAX_WITHDRAWAL_SETTLEMENTS_PER_ENTRY};
    AppendSettlementBlock(index, fixture, *checkpoint_id, C0 + 1,
                          /*first_number=*/0, LIMIT + 1);

    int selected_height{-1};
    size_t selected_count{0};
    std::string error;
    BOOST_CHECK(!index.WithdrawalSettlementCatchupHeight(
        fixture.market, fixture.chain.indexes[C0],
        fixture.chain.indexes[C0 + 1], LIMIT, selected_height,
        selected_count, error));
    BOOST_CHECK(error.find("one B3 block exceeds") != std::string::npos);

    std::vector<flowmesh::WithdrawalSettlementFactV1> settlements;
    BOOST_CHECK(!index.WithdrawalSettlementsBetween(
        fixture.market, fixture.chain.indexes[C0],
        fixture.chain.indexes[C0 + 1], settlements, error));
    BOOST_CHECK(settlements.empty());
}

BOOST_AUTO_TEST_CASE(unique_bootstrap_anchor_rejects_alternates_and_survives_delay)
{
    const Fixture fixture;
    node::FlowMeshCheckpointIndex index;
    node::FlowMeshCheckpointBlockDelta ignored;
    std::string error;

    // The market does not exist before A3, so an otherwise valid older
    // committee cannot bootstrap it.
    const auto old_anchor{fixture.Genesis(A3 - 1)};
    BOOST_CHECK(!index.VerifyBlock(
        Block({Tx({fixture.CheckpointRecord(old_anchor)})}), C0,
        Filled(0xa7), fixture.chain.chain, fixture.params, fixture.seats,
        fixture.vaults, ignored, error));
    BOOST_CHECK_EQUAL(
        error,
        "FlowMesh checkpoint market is not registered at its production anchor");

    // A3 is the first post-market block with four seats. Even after A3+1 is
    // deep and has a different valid committee, it is not a second genesis.
    const auto new_anchor{fixture.Genesis(A3 + 1)};
    BOOST_CHECK(!index.VerifyBlock(
        Block({Tx({fixture.CheckpointRecord(new_anchor)})}), C0 + 1,
        Filled(0xa8), fixture.chain.chain, fixture.params, fixture.seats,
        fixture.vaults, ignored, error));
    BOOST_CHECK_EQUAL(
        error,
        "first FlowMesh checkpoint does not use the unique bootstrap anchor");

    // The exact A3 genesis remains valid after unrelated publication delays;
    // discovery time and the latest signable committee cannot replace it.
    const auto first{fixture.Genesis(A3)};
    const CBlock block{Block({Tx({fixture.CheckpointRecord(first)})})};

    for (const int delay : {1, 2, 10}) {
        node::FlowMeshCheckpointIndex delayed_index;
        const auto delta{Verify(delayed_index, fixture, block, C0 + delay,
                                static_cast<unsigned char>(0xa8 + delay))};
        BOOST_REQUIRE_EQUAL(delta.checkpoints.size(), 1U);
        BOOST_CHECK(delta.checkpoints[0].core.anchor.height ==
                    static_cast<uint64_t>(A3));
    }
}

BOOST_AUTO_TEST_CASE(handoff_survives_unrelated_inclusion_delays)
{
    const Fixture fixture;
    const auto first{fixture.Genesis(A3)};
    const auto first_id{modern::FlowMeshCheckpointIdV1(first)};
    BOOST_REQUIRE(first_id.has_value());

    auto handoff{fixture.Core(A3, 0, 1, *first_id, {}, 0)};
    handoff.kind = modern::FlowMeshCheckpointKind::EPOCH_HANDOFF;
    handoff.handoff = modern::FlowMeshCheckpointHandoffV1{
        1,
        {static_cast<uint64_t>(A3 + 1), fixture.chain.hashes[A3 + 1]},
        fixture.Active(A3 + 1, 1).set_hash};
    Fixture::BindProductionIdentity(handoff);
    const CBlock handoff_block{
        Block({Tx({fixture.CheckpointRecord(handoff)})})};

    // The handoff was signable for C0+1. Its target membership remains active
    // while unrelated blocks delay publication by one, two, or ten blocks.
    for (const int delay : {1, 2, 10}) {
        node::FlowMeshCheckpointIndex index;
        Connect(index, Verify(index, fixture,
                              Block({Tx({fixture.CheckpointRecord(first)})}),
                              C0, static_cast<unsigned char>(0xb8 + delay)));
        const auto delta{Verify(index, fixture, handoff_block,
                                C0 + 1 + delay,
                                static_cast<unsigned char>(0xc8 + delay))};
        BOOST_REQUIRE_EQUAL(delta.checkpoints.size(), 1U);
        BOOST_CHECK(delta.checkpoints[0].core.kind ==
                    modern::FlowMeshCheckpointKind::EPOCH_HANDOFF);
    }
}

BOOST_AUTO_TEST_CASE(delayed_genesis_is_exact_and_handoff_rejects_stale_membership)
{
    const Fixture fixture;
    node::FlowMeshCheckpointIndex index;
    node::FlowMeshCheckpointBlockDelta ignored;
    std::string error;

    // An arbitrary sequence-zero root cannot use delayed publication as a
    // way to invent a market state under an old committee.
    const auto arbitrary_first{fixture.Core(A3, 0, 0, {}, {}, 0)};
    BOOST_CHECK(!index.VerifyBlock(
        Block({Tx({fixture.CheckpointRecord(arbitrary_first)})}), C0 + 1,
        Filled(0xd8), fixture.chain.chain, fixture.params, fixture.seats,
        fixture.vaults, ignored, error));
    BOOST_CHECK_EQUAL(
        error, "first FlowMesh checkpoint is not the deterministic empty genesis");

    // A fabricated nonempty effect range is invalid even when its compact
    // identity and BLS certificate are internally consistent.
    auto nonempty{fixture.Genesis(A3)};
    const std::vector<modern::FlowMeshEffectV1> effects{fixture.Deposit(0)};
    nonempty.effect_count = 1;
    nonempty.effect_root = *modern::ComputeFlowMeshEffectRoot(0, effects);
    Fixture::BindProductionIdentity(nonempty);
    BOOST_CHECK(!index.VerifyBlock(
        Block({Tx({fixture.CheckpointRecord(nonempty)})}), C0 + 1,
        Filled(0xd7), fixture.chain.chain, fixture.params, fixture.seats,
        fixture.vaults, ignored, error));
    BOOST_CHECK_EQUAL(
        error, "first FlowMesh checkpoint is not the deterministic empty genesis");

    // A3's set is no longer latest once the A3+1 rotation is signable, but
    // its unique empty genesis was durably certified and remains publishable.
    const auto delayed_genesis{fixture.Genesis(A3)};
    Connect(index, Verify(index, fixture,
                          Block({Tx({fixture.CheckpointRecord(delayed_genesis)})}),
                          C0 + 1, 0xd6));
    BOOST_REQUIRE(index.Head(fixture.market).has_value());
    BOOST_CHECK(index.Head(fixture.market)->core == delayed_genesis);

    // A handoff target that was current when signed is likewise stale if a
    // later membership change becomes safely signable before publication.
    const Fixture twice_rotated{A3 + 5};
    node::FlowMeshCheckpointIndex handoff_index;
    const auto first{twice_rotated.Genesis(A3)};
    const auto first_id{modern::FlowMeshCheckpointIdV1(first)};
    BOOST_REQUIRE(first_id.has_value());
    Connect(handoff_index,
            Verify(handoff_index, twice_rotated,
                   Block({Tx({twice_rotated.CheckpointRecord(first)})}), C0,
                   0xd9));

    auto stale_handoff{
        twice_rotated.Core(A3, 0, 1, *first_id, {}, 0)};
    stale_handoff.kind = modern::FlowMeshCheckpointKind::EPOCH_HANDOFF;
    stale_handoff.handoff = modern::FlowMeshCheckpointHandoffV1{
        1,
        {static_cast<uint64_t>(A3 + 1),
         twice_rotated.chain.hashes[A3 + 1]},
        twice_rotated.Active(A3 + 1, 1).set_hash};
    Fixture::BindProductionIdentity(stale_handoff);
    BOOST_CHECK(!handoff_index.VerifyBlock(
        Block({Tx({twice_rotated.CheckpointRecord(stale_handoff)})}), C0 + 5,
        Filled(0xda), twice_rotated.chain.chain, twice_rotated.params,
        twice_rotated.seats, twice_rotated.vaults, ignored, error));
    BOOST_CHECK_EQUAL(
        error, "FlowMesh handoff names obsolete next seat membership");
}

BOOST_AUTO_TEST_CASE(connected_head_drains_old_set_before_exact_handoff)
{
    const Fixture fixture;
    node::FlowMeshCheckpointIndex index;
    const auto first{fixture.Genesis(A3)};
    const auto first_id{modern::FlowMeshCheckpointIdV1(first)};
    BOOST_REQUIRE(first_id.has_value());
    Connect(index, Verify(index, fixture,
                          Block({Tx({fixture.CheckpointRecord(first)})}),
                          C0, 0xb1));

    // A different membership becomes safely signable at C0+1, after this
    // old-set execution was already certified. The connected head remains
    // authoritative so the pending checkpoint can drain before handoff.
    const auto pending{fixture.Core(A3, 0, 1, *first_id, {}, 0)};
    const auto pending_id{modern::FlowMeshCheckpointIdV1(pending)};
    BOOST_REQUIRE(pending_id.has_value());
    Connect(index, Verify(index, fixture,
                          Block({Tx({fixture.CheckpointRecord(pending)})}),
                          C0 + 1, 0xb0));
    BOOST_REQUIRE(index.Head(fixture.market).has_value());
    BOOST_CHECK(index.Head(fixture.market)->checkpoint_id == *pending_id);

    node::FlowMeshCheckpointBlockDelta ignored;
    std::string error;
    // Direct epoch/set replacement is forbidden.
    const auto bypass{fixture.Core(A3 + 1, 1, 2, *pending_id, {}, 0)};
    BOOST_CHECK(!index.VerifyBlock(
        Block({Tx({fixture.CheckpointRecord(bypass)})}), C0 + 2,
        Filled(0xb2), fixture.chain.chain, fixture.params, fixture.seats,
        fixture.vaults, ignored, error));
    BOOST_CHECK_EQUAL(error,
                      "FlowMesh epoch/anchor/set changed without a connected handoff");

    // The eventual handoff must target the latest signable membership; the
    // outgoing connected committee cannot choose an intermediate set.
    auto handoff{fixture.Core(A3, 0, 2, *pending_id, {}, 0)};
    handoff.kind = modern::FlowMeshCheckpointKind::EPOCH_HANDOFF;
    handoff.handoff = modern::FlowMeshCheckpointHandoffV1{
        1,
        {static_cast<uint64_t>(A3 + 1), fixture.chain.hashes[A3 + 1]},
        fixture.Active(A3 + 1, 1).set_hash};
    Fixture::BindProductionIdentity(handoff);
    const auto handoff_id{modern::FlowMeshCheckpointIdV1(handoff)};
    BOOST_REQUIRE(handoff_id.has_value());
    Connect(index, Verify(index, fixture,
                          Block({Tx({fixture.CheckpointRecord(handoff)})}),
                          C0 + 2, 0xb3));

    auto wrong_next{fixture.Core(A3, 1, 3, *handoff_id, {}, 0)};
    BOOST_CHECK(!index.VerifyBlock(
        Block({Tx({fixture.CheckpointRecord(wrong_next)})}), C0 + 3,
        Filled(0xb4), fixture.chain.chain, fixture.params, fixture.seats,
        fixture.vaults, ignored, error));
    BOOST_CHECK_EQUAL(error,
                      "FlowMesh execution does not activate the connected handoff");

    const auto next{fixture.Core(A3 + 1, 1, 3, *handoff_id, {}, 0)};
    Connect(index, Verify(index, fixture,
                          Block({Tx({fixture.CheckpointRecord(next)})}),
                          C0 + 3, 0xb5));
    BOOST_REQUIRE(index.Head(fixture.market).has_value());
    BOOST_CHECK_EQUAL(index.Head(fixture.market)->core.epoch, 1U);
}

BOOST_AUTO_TEST_CASE(first_checkpoint_is_an_execution_at_zero_on_one_anchor)
{
    const Fixture fixture;
    node::FlowMeshCheckpointIndex index;
    node::FlowMeshCheckpointBlockDelta ignored;
    std::string error;

    // Neither the certificate nor the deterministic genesis covers
    // previous_checkpoint_id, so anyone holding the honest certified genesis
    // can relabel it.
    const CMpaRecord honest{fixture.CheckpointRecord(fixture.Genesis(A3))};
    const CMpaRecord relabelled{
        Relabelled(honest, fixture.Active(A3, 0).Size(), Filled(0x6b))};
    BOOST_CHECK(!index.VerifyBlock(
        Block({Tx({relabelled})}), C0, Filled(0xe3), fixture.chain.chain,
        fixture.params, fixture.seats, fixture.vaults, ignored, error));
    BOOST_CHECK_EQUAL(error,
                      "first FlowMesh checkpoint does not name the null head");

    // A quorum can certify an empty genesis at another epoch, or produced at
    // a later anchor than its seats. The rebuilt genesis takes its epoch from
    // the same seat set and its production anchor from the record, so only
    // these two rules refuse them.
    const auto later_epoch{fixture.Genesis(A3, 5)};
    BOOST_CHECK(!index.VerifyBlock(
        Block({Tx({fixture.CheckpointRecord(later_epoch)})}), C0 + 1,
        Filled(0xe4), fixture.chain.chain, fixture.params, fixture.seats,
        fixture.vaults, ignored, error));
    BOOST_CHECK_EQUAL(error, "first FlowMesh checkpoint must use epoch zero");

    const auto later_production{fixture.Genesis(A3, 0, A3 + 1)};
    BOOST_CHECK(!index.VerifyBlock(
        Block({Tx({fixture.CheckpointRecord(later_production)})}), C0 + 1,
        Filled(0xe6), fixture.chain.chain, fixture.params, fixture.seats,
        fixture.vaults, ignored, error));
    BOOST_CHECK_EQUAL(
        error, "first FlowMesh checkpoint must use one seat/production anchor");

    // The rebuilt genesis is an execution at sequence zero whose effects
    // start at zero, so it refuses the next three records too; their own
    // rules refuse them first.
    auto first_handoff{fixture.Genesis(A3)};
    first_handoff.kind = modern::FlowMeshCheckpointKind::EPOCH_HANDOFF;
    first_handoff.handoff = modern::FlowMeshCheckpointHandoffV1{
        1, {static_cast<uint64_t>(A3 + 1), fixture.chain.hashes[A3 + 1]},
        fixture.Active(A3 + 1, 1).set_hash};
    BOOST_CHECK(!index.VerifyBlock(
        Block({Tx({fixture.CheckpointRecord(first_handoff)})}), C0 + 1,
        Filled(0xe0), fixture.chain.chain, fixture.params, fixture.seats,
        fixture.vaults, ignored, error));
    BOOST_CHECK_EQUAL(error,
                      "first FlowMesh checkpoint must be an execution checkpoint");

    auto later_sequence{fixture.Genesis(A3)};
    later_sequence.sequence = 1;
    later_sequence.parent_hash = Filled(0x81); // a later entry has a parent
    BOOST_CHECK(!index.VerifyBlock(
        Block({Tx({fixture.CheckpointRecord(later_sequence)})}), C0 + 1,
        Filled(0xe1), fixture.chain.chain, fixture.params, fixture.seats,
        fixture.vaults, ignored, error));
    BOOST_CHECK_EQUAL(error, "first FlowMesh checkpoint must use sequence zero");

    auto later_effects{fixture.Genesis(A3)};
    later_effects.effect_start = 1;
    later_effects.effect_root = modern::EmptyFlowMeshEffectRoot(1);
    BOOST_CHECK(!index.VerifyBlock(
        Block({Tx({fixture.CheckpointRecord(later_effects)})}), C0 + 1,
        Filled(0xe2), fixture.chain.chain, fixture.params, fixture.seats,
        fixture.vaults, ignored, error));
    BOOST_CHECK_EQUAL(
        error, "first FlowMesh checkpoint effect range does not start at zero");

    // The honest record, identical to the relabelled one but for its
    // previous id, is the market's genesis.
    BOOST_CHECK_EQUAL(Verify(index, fixture, Block({Tx({honest})}), C0, 0xe8)
                          .checkpoints.size(),
                      1U);
}

BOOST_AUTO_TEST_CASE(relabelled_checkpoint_cannot_rewind_the_head)
{
    const Fixture fixture;
    node::FlowMeshCheckpointIndex index;
    node::FlowMeshCheckpointBlockDelta ignored;

    // Genesis and two empty checkpoints on one anchor.
    const auto first{fixture.Genesis(A3)};
    const auto first_id{modern::FlowMeshCheckpointIdV1(first)};
    BOOST_REQUIRE(first_id.has_value());
    const auto second{fixture.Core(A3, 0, 1, *first_id, {}, 0)};
    const auto second_id{modern::FlowMeshCheckpointIdV1(second)};
    BOOST_REQUIRE(second_id.has_value());
    const auto third{fixture.Core(A3, 0, 2, *second_id, {}, 0)};
    const auto third_id{modern::FlowMeshCheckpointIdV1(third)};
    BOOST_REQUIRE(third_id.has_value());
    const CMpaRecord second_record{fixture.CheckpointRecord(second)};
    const CMpaRecord third_record{fixture.CheckpointRecord(third)};
    Connect(index, Verify(index, fixture,
                          Block({Tx({fixture.CheckpointRecord(first),
                                     second_record, third_record})}),
                          C0, 0xfa));

    // Relabelled to extend the head, a connected record still fits its
    // effect range, anchors and seats. Only the sequence rule stops anyone
    // from rewinding the market to that record's state, or repeating the
    // head itself.
    const size_t seat_count{fixture.Active(A3, 0).Size()};
    const CMpaRecord older{Relabelled(second_record, seat_count, *third_id)};
    std::string older_error;
    BOOST_CHECK(!index.VerifyBlock(
        Block({Tx({older})}), C0 + 1, Filled(0xfb), fixture.chain.chain,
        fixture.params, fixture.seats, fixture.vaults, ignored, older_error));
    BOOST_CHECK_EQUAL(older_error,
                      "FlowMesh checkpoint sequence is not strictly increasing");
    const CMpaRecord repeated{Relabelled(third_record, seat_count, *third_id)};
    std::string repeated_error;
    BOOST_CHECK(!index.VerifyBlock(
        Block({Tx({repeated})}), C0 + 1, Filled(0xfc), fixture.chain.chain,
        fixture.params, fixture.seats, fixture.vaults, ignored,
        repeated_error));
    BOOST_CHECK_EQUAL(repeated_error,
                      "FlowMesh checkpoint sequence is not strictly increasing");
}

BOOST_AUTO_TEST_CASE(handoff_names_its_next_set_and_changes_membership)
{
    // Keys rotate at A3+1 and again at A3+5: A3+1..A3+4 hold one membership.
    const Fixture fixture{A3 + 5};
    node::FlowMeshCheckpointIndex index;
    node::FlowMeshCheckpointBlockDelta ignored;
    std::string error;

    const auto first{fixture.Genesis(A3)};
    const auto first_id{modern::FlowMeshCheckpointIdV1(first)};
    BOOST_REQUIRE(first_id.has_value());
    Connect(index, Verify(index, fixture,
                          Block({Tx({fixture.CheckpointRecord(first)})}),
                          C0, 0xe9));

    auto rotate{fixture.Core(A3, 0, 1, *first_id, {}, 0)};
    rotate.kind = modern::FlowMeshCheckpointKind::EPOCH_HANDOFF;
    rotate.handoff = modern::FlowMeshCheckpointHandoffV1{
        1,
        {static_cast<uint64_t>(A3 + 1), fixture.chain.hashes[A3 + 1]},
        fixture.Active(A3 + 1, 1).set_hash};
    Fixture::BindProductionIdentity(rotate);

    // A next set hash that is not the anchored set's (here the right seats
    // hashed for the wrong epoch) would strand the market: no execution
    // could ever activate it.
    auto wrong_set{rotate};
    wrong_set.handoff->next_seat_set_hash = fixture.Active(A3 + 1, 2).set_hash;
    Fixture::BindProductionIdentity(wrong_set);
    BOOST_CHECK(!index.VerifyBlock(
        Block({Tx({fixture.CheckpointRecord(wrong_set)})}), C0 + 1,
        Filled(0xea), fixture.chain.chain, fixture.params, fixture.seats,
        fixture.vaults, ignored, error));
    BOOST_CHECK_EQUAL(error,
                      "FlowMesh handoff names the wrong next anchored seat set");

    const auto rotate_id{modern::FlowMeshCheckpointIdV1(rotate)};
    BOOST_REQUIRE(rotate_id.has_value());
    Connect(index, Verify(index, fixture,
                          Block({Tx({fixture.CheckpointRecord(rotate)})}),
                          C0 + 1, 0xeb));
    const auto activate{fixture.Core(A3 + 1, 1, 2, *rotate_id, {}, 0)};
    const auto activate_id{modern::FlowMeshCheckpointIdV1(activate)};
    BOOST_REQUIRE(activate_id.has_value());
    Connect(index, Verify(index, fixture,
                          Block({Tx({fixture.CheckpointRecord(activate)})}),
                          C0 + 2, 0xec));

    // While epoch 1's keys are still the latest signable membership, it
    // cannot hand off to the same keys (A3+2 holds them too). A block at
    // C0+4 still signs against A3+4: the latest signable seats are those
    // FLOWMESH_ANCHOR_DEPTH + 1 blocks below it.
    auto same_keys{fixture.Core(A3 + 1, 1, 3, *activate_id, {}, 0)};
    same_keys.kind = modern::FlowMeshCheckpointKind::EPOCH_HANDOFF;
    same_keys.handoff = modern::FlowMeshCheckpointHandoffV1{
        2,
        {static_cast<uint64_t>(A3 + 2), fixture.chain.hashes[A3 + 2]},
        fixture.Active(A3 + 2, 2).set_hash};
    Fixture::BindProductionIdentity(same_keys);
    BOOST_CHECK(!index.VerifyBlock(
        Block({Tx({fixture.CheckpointRecord(same_keys)})}), C0 + 4,
        Filled(0xed), fixture.chain.chain, fixture.params, fixture.seats,
        fixture.vaults, ignored, error));
    BOOST_CHECK_EQUAL(
        error, "FlowMesh handoff does not change the active seat membership");

    // From C0+5 the A3+5 rotation is signable, and epoch 1 hands off to it.
    auto rotate_again{same_keys};
    rotate_again.handoff = modern::FlowMeshCheckpointHandoffV1{
        2,
        {static_cast<uint64_t>(A3 + 5), fixture.chain.hashes[A3 + 5]},
        fixture.Active(A3 + 5, 2).set_hash};
    Fixture::BindProductionIdentity(rotate_again);
    BOOST_CHECK_EQUAL(
        Verify(index, fixture,
               Block({Tx({fixture.CheckpointRecord(rotate_again)})}), C0 + 5,
               0xee)
            .checkpoints.size(),
        1U);
}

BOOST_AUTO_TEST_CASE(checkpoint_anchors_name_canonical_blocks)
{
    const Fixture fixture;
    node::FlowMeshCheckpointIndex index;
    node::FlowMeshCheckpointBlockDelta ignored;
    std::string error;

    const auto first{fixture.Genesis(A3)};
    const auto first_id{modern::FlowMeshCheckpointIdV1(first)};
    BOOST_REQUIRE(first_id.has_value());
    Connect(index, Verify(index, fixture,
                          Block({Tx({fixture.CheckpointRecord(first)})}),
                          C0, 0xef));

    // The records below are verified in a block at C0+1. The newest anchor
    // such a block can name is exactly FLOWMESH_ANCHOR_DEPTH below it.
    const int candidate{C0 + 1};
    const int newest{candidate - Consensus::FLOWMESH_ANCHOR_DEPTH};

    // Anchors are that deep, but a deeper reorganization can still abandon
    // one. A committee that certified on the abandoned branch names that
    // branch's block at a canonical height. It checked its deposits against
    // that block, so its effects may credit deposits that do not exist on
    // this chain.
    const uint256 abandoned{Filled(0x6f)};
    auto off_chain{fixture.Core(A3, 0, 1, *first_id, {}, 0)};
    off_chain.production_anchor = {static_cast<uint64_t>(newest), abandoned};
    Fixture::BindProductionIdentity(off_chain);
    BOOST_CHECK(!index.VerifyBlock(
        Block({Tx({fixture.CheckpointRecord(off_chain)})}), candidate,
        Filled(0xf3), fixture.chain.chain, fixture.params, fixture.seats,
        fixture.vaults, ignored, error));
    BOOST_CHECK_EQUAL(error,
                      "FlowMesh checkpoint production anchor is not canonical");

    // The same holds for the seats a handoff names: the abandoned block's
    // seats here equal the canonical block's, only its hash differs.
    auto off_chain_seats{fixture.Core(A3, 0, 1, *first_id, {}, 0)};
    off_chain_seats.kind = modern::FlowMeshCheckpointKind::EPOCH_HANDOFF;
    off_chain_seats.handoff = modern::FlowMeshCheckpointHandoffV1{
        1, {static_cast<uint64_t>(newest), abandoned},
        fixture.Active(newest, 1, abandoned).set_hash};
    Fixture::BindProductionIdentity(off_chain_seats);
    BOOST_CHECK(!index.VerifyBlock(
        Block({Tx({fixture.CheckpointRecord(off_chain_seats)})}), candidate,
        Filled(0xf7), fixture.chain.chain, fixture.params, fixture.seats,
        fixture.vaults, ignored, error));
    BOOST_CHECK_EQUAL(error,
                      "FlowMesh checkpoint seat-set anchor is not canonical");

    // One block newer is not deep enough.
    auto shallow{fixture.Core(A3, 0, 1, *first_id, {}, 0)};
    shallow.production_anchor = {static_cast<uint64_t>(newest + 1),
                                 fixture.chain.hashes[newest + 1]};
    Fixture::BindProductionIdentity(shallow);
    BOOST_CHECK(!index.VerifyBlock(
        Block({Tx({fixture.CheckpointRecord(shallow)})}), candidate,
        Filled(0xfd), fixture.chain.chain, fixture.params, fixture.seats,
        fixture.vaults, ignored, error));
    BOOST_CHECK_EQUAL(error,
                      "FlowMesh checkpoint production anchor is not deep enough");

    // Named by its canonical hash, the newest anchor is accepted for both.
    auto on_chain_seats{off_chain_seats};
    on_chain_seats.handoff->next_anchor.block_hash =
        fixture.chain.hashes[newest];
    on_chain_seats.handoff->next_seat_set_hash =
        fixture.Active(newest, 1).set_hash;
    Fixture::BindProductionIdentity(on_chain_seats);
    BOOST_CHECK_EQUAL(
        Verify(index, fixture,
               Block({Tx({fixture.CheckpointRecord(on_chain_seats)})}),
               candidate, 0xf9)
            .checkpoints.size(),
        1U);
    auto on_chain{off_chain};
    on_chain.production_anchor.block_hash = fixture.chain.hashes[newest];
    Fixture::BindProductionIdentity(on_chain);
    const auto on_chain_id{modern::FlowMeshCheckpointIdV1(on_chain)};
    BOOST_REQUIRE(on_chain_id.has_value());
    Connect(index, Verify(index, fixture,
                          Block({Tx({fixture.CheckpointRecord(on_chain)})}),
                          candidate, 0xf8));

    // Production anchors never move back: the next checkpoint cannot be
    // produced against an older B3 view than its head's.
    const auto behind{fixture.Core(A3, 0, 2, *on_chain_id, {}, 0)};
    BOOST_CHECK(!index.VerifyBlock(
        Block({Tx({fixture.CheckpointRecord(behind)})}), candidate + 1,
        Filled(0xfe), fixture.chain.chain, fixture.params, fixture.seats,
        fixture.vaults, ignored, error));
    BOOST_CHECK_EQUAL(error,
                      "FlowMesh checkpoint production anchor is not monotonic");
}

BOOST_AUTO_TEST_CASE(checkpoint_names_its_chain_domain_and_anchored_seat_set)
{
    const Fixture fixture;
    node::FlowMeshCheckpointIndex index;
    node::FlowMeshCheckpointBlockDelta ignored;
    std::string error;

    const auto first{fixture.Genesis(A3)};
    const auto first_id{modern::FlowMeshCheckpointIdV1(first)};
    BOOST_REQUIRE(first_id.has_value());
    Connect(index, Verify(index, fixture,
                          Block({Tx({fixture.CheckpointRecord(first)})}),
                          C0, 0xd0));

    // Each record below is the head's certified successor with one field
    // changed after signing. Later checks refuse both too, the certificate
    // check among them, but these rules refuse them first.
    const CMpaRecord next{
        fixture.CheckpointRecord(fixture.Core(A3, 0, 1, *first_id))};
    const size_t seat_count{fixture.Active(A3, 0).Size()};
    const CMpaRecord other_domain{Altered(
        next, seat_count, [](modern::FlowMeshCheckpointCoreV1& core) {
            core.domain = Filled(0x6e);
        })};
    BOOST_CHECK(!index.VerifyBlock(
        Block({Tx({other_domain})}), C0 + 1, Filled(0xd1),
        fixture.chain.chain, fixture.params, fixture.seats, fixture.vaults,
        ignored, error));
    BOOST_CHECK_EQUAL(error, "FlowMesh checkpoint names the wrong chain domain");

    // Another anchor's seats are a real set, but not this anchor's.
    const CMpaRecord other_seats{Altered(
        next, seat_count, [&](modern::FlowMeshCheckpointCoreV1& core) {
            core.seat_set_hash = fixture.Active(A3 + 1, 0).set_hash;
        })};
    BOOST_CHECK(!index.VerifyBlock(
        Block({Tx({other_seats})}), C0 + 1, Filled(0xd2),
        fixture.chain.chain, fixture.params, fixture.seats, fixture.vaults,
        ignored, error));
    BOOST_CHECK_EQUAL(error,
                      "FlowMesh checkpoint names the wrong anchored seat set");

    // Unchanged, the record extends the head.
    BOOST_CHECK_EQUAL(
        Verify(index, fixture, Block({Tx({next})}), C0 + 1, 0xd3)
            .checkpoints.size(),
        1U);
}

BOOST_AUTO_TEST_CASE(vault_proof_requires_a_connected_checkpoint_and_nullifies_once)
{
    const Fixture fixture;
    node::FlowMeshCheckpointIndex index;
    const auto genesis{fixture.Genesis(A3)};
    const auto genesis_id{modern::FlowMeshCheckpointIdV1(genesis)};
    BOOST_REQUIRE(genesis_id.has_value());
    const modern::FlowMeshEffectV1 deposit{fixture.Deposit(1)};
    const std::vector<modern::FlowMeshEffectV1> effects{deposit};
    const auto core{fixture.Core(A3, 0, 1, *genesis_id, effects, 0)};
    const auto checkpoint_id{modern::FlowMeshCheckpointIdV1(core)};
    BOOST_REQUIRE(checkpoint_id.has_value());
    modern::FlowMeshVaultProofV1 proof;
    proof.kind = modern::FlowMeshVaultProofKind::DEPOSIT_SWEEP;
    proof.checkpoint_id = *checkpoint_id;
    proof.effect = deposit;
    proof.leaf_index = 0;
    const auto branch{modern::BuildFlowMeshEffectBranch(0, effects, 0)};
    BOOST_REQUIRE(branch.has_value());
    proof.branch = *branch;
    const auto proof_bytes{modern::EncodeFlowMeshVaultProofV1(proof)};
    BOOST_REQUIRE(proof_bytes.has_value());
    const CMpaRecord proof_record{modern::MPA_TYPE_FLOWMESH_VAULT_PROOF,
                                  modern::MPA_VERSION_V1, *proof_bytes};

    node::FlowMeshCheckpointBlockDelta delta;
    std::string error;
    // A checkpoint and its spend proof cannot bootstrap each other atomically
    // inside one block: type 9 requires a prior connected checkpoint.
    BOOST_CHECK(!index.VerifyBlock(
        Block({Tx({fixture.CheckpointRecord(genesis),
                   fixture.CheckpointRecord(core)}),
               Tx({proof_record})}),
        C0, Filled(0xc1), fixture.chain.chain, fixture.params,
        fixture.seats, fixture.vaults, delta, error));
    BOOST_CHECK_EQUAL(error,
                      "FlowMesh vault proof checkpoint is not connected");

    Connect(index, Verify(index, fixture,
                          Block({Tx({fixture.CheckpointRecord(genesis),
                                     fixture.CheckpointRecord(core)})}),
                          C0, 0xc2));

    CMutableTransaction coinbase;
    coinbase.version = 2;
    coinbase.vin.emplace_back();
    coinbase.mpa = {proof_record};
    BOOST_REQUIRE(CTransaction{coinbase}.IsCoinBase());
    BOOST_CHECK(!index.VerifyBlock(
        Block({MakeTransactionRef(coinbase)}), C0 + 1, Filled(0xcf),
        fixture.chain.chain, fixture.params, fixture.seats, fixture.vaults,
        delta, error));
    BOOST_CHECK_EQUAL(error,
                      "FlowMesh vault proof is not allowed in coinbase");

    const auto spend_delta{Verify(index, fixture, Block({Tx({proof_record})}),
                                  C0 + 1, 0xc3)};
    BOOST_REQUIRE_EQUAL(spend_delta.nullifiers.size(), 1U);
    // Two uses in one block must fail the block rule itself: the index's own
    // duplicate check runs only in the connect hook, after the block's coins
    // have moved.
    BOOST_CHECK(!index.VerifyBlock(
        Block({Tx({proof_record}, 1), Tx({proof_record}, 2)}), C0 + 1,
        Filled(0xc6), fixture.chain.chain, fixture.params, fixture.seats,
        fixture.vaults, delta, error));
    BOOST_CHECK_EQUAL(error, "FlowMesh vault effect is already nullified");
    Connect(index, spend_delta);
    BOOST_CHECK(index.IsNullified(spend_delta.nullifiers[0]));

    BOOST_CHECK(!index.VerifyBlock(Block({Tx({proof_record})}), C0 + 2,
                                   Filled(0xc4), fixture.chain.chain,
                                   fixture.params, fixture.seats,
                                   fixture.vaults, delta, error));
    BOOST_CHECK_EQUAL(error, "FlowMesh vault effect is already nullified");

    BOOST_REQUIRE(index.DisconnectBlock(C0 + 1, Filled(0xc3), error));
    BOOST_CHECK(!index.IsNullified(spend_delta.nullifiers[0]));
    BOOST_CHECK(index.VerifyBlock(Block({Tx({proof_record})}), C0 + 1,
                                  Filled(0xc5), fixture.chain.chain,
                                  fixture.params, fixture.seats,
                                  fixture.vaults, delta, error));
}

BOOST_AUTO_TEST_CASE(activation_anchor_bitmap_and_preview_fail_closed)
{
    Fixture fixture;
    node::FlowMeshCheckpointIndex index;
    const auto core{fixture.Genesis(A3)};
    const auto tx{Tx({fixture.CheckpointRecord(core)})};
    std::string error;

    BOOST_CHECK(!index.VerifyTransaction(*tx, A3 - 1, fixture.chain.chain,
                                         fixture.params, fixture.seats,
                                         fixture.vaults, error));
    BOOST_CHECK_EQUAL(error, "FlowMesh checkpoint index is not active");
    BOOST_CHECK(index.VerifyTransaction(*tx, C0, fixture.chain.chain,
                                        fixture.params, fixture.seats,
                                        fixture.vaults, error));
    BOOST_CHECK_EQUAL(index.CheckpointCount(), 0U); // preview never mutates

    auto shallow{fixture.Core(A3 + 1, 0, 2, {}, {}, 0)};
    node::FlowMeshCheckpointBlockDelta delta;
    BOOST_CHECK(!index.VerifyBlock(
        Block({Tx({fixture.CheckpointRecord(shallow)})}), A3,
        Filled(0xd1), fixture.chain.chain, fixture.params, fixture.seats,
        fixture.vaults, delta, error));
    BOOST_CHECK_EQUAL(error,
                      "FlowMesh checkpoint production anchor is not deep enough");

    auto bitmap{fixture.CheckpointRecord(core)};
    const size_t core_size{modern::FLOWMESH_EXECUTION_CHECKPOINT_CORE_V1_SIZE};
    bitmap.payload[core_size] |= 0x80; // high bit beyond four seats
    BOOST_CHECK(!index.VerifyBlock(Block({Tx({bitmap})}), C0,
                                   Filled(0xd2), fixture.chain.chain,
                                   fixture.params, fixture.seats,
                                   fixture.vaults, delta, error));
    BOOST_CHECK_EQUAL(
        error, "FlowMesh checkpoint has a non-canonical signer bitmap");
}

BOOST_AUTO_TEST_CASE(deposit_sweep_is_exact_and_fee_is_owner_funded)
{
    const Fixture fixture;
    node::FlowMeshCheckpointIndex index;
    const auto genesis{fixture.Genesis(A3)};
    const auto genesis_id{modern::FlowMeshCheckpointIdV1(genesis)};
    BOOST_REQUIRE(genesis_id.has_value());
    const modern::FlowMeshDepositAcceptanceV1 acceptance{fixture.Deposit(1)};
    const std::vector<modern::FlowMeshEffectV1> effects{acceptance};
    const auto core{fixture.Core(A3, 0, 1, *genesis_id, effects, 0)};
    const auto checkpoint_id{modern::FlowMeshCheckpointIdV1(core)};
    BOOST_REQUIRE(checkpoint_id.has_value());
    Connect(index, Verify(index, fixture,
                          Block({Tx({fixture.CheckpointRecord(genesis),
                                     fixture.CheckpointRecord(core)})}),
                          C0, 0xe1));

    modern::FlowMeshVaultProofV1 proof;
    proof.kind = modern::FlowMeshVaultProofKind::DEPOSIT_SWEEP;
    proof.checkpoint_id = *checkpoint_id;
    proof.effect = acceptance;
    proof.leaf_index = 0;
    proof.branch = *modern::BuildFlowMeshEffectBranch(0, effects, 0);

    const auto deposit{modern::MakeDexVaultOutput(
        acceptance.asset, acceptance.amount, acceptance.vault_id,
        modern::VAULT_KIND_USER_DEPOSIT, acceptance.shard,
        acceptance.account)};
    const auto pool{modern::MakeDexVaultOutput(
        acceptance.asset, acceptance.amount, acceptance.vault_id,
        modern::VAULT_KIND_POOL_CHANGE, acceptance.shard)};
    BOOST_REQUIRE(deposit.has_value());
    BOOST_REQUIRE(pool.has_value());

    CMutableTransaction spend;
    spend.version = 2;
    spend.vin.emplace_back(acceptance.deposit_outpoint);
    const COutPoint fee_outpoint{Txid::FromUint256(Filled(0xe2)), 0};
    spend.vin.emplace_back(fee_outpoint);
    spend.vout = {*pool, CTxOut{9, CScript() << OP_TRUE}};
    spend.mpa = {ProofRecord(proof)};
    const std::vector<Coin> prevs{
        Coin{*deposit, A3, /*coinbase=*/false},
        Coin{CTxOut{10, CScript() << OP_TRUE}, A3,
             /*coinbase=*/false}};

    node::FlowMeshVaultAuthorization authorization;
    std::string error;
    BOOST_REQUIRE_MESSAGE(node::CheckFlowMeshVaultTransaction(
                              CTransaction{spend}, prevs, 1, C0 + 1,
                              fixture.params, index, authorization, error),
                          error);
    BOOST_REQUIRE_EQUAL(authorization.authorized_inputs.size(), 2U);
    BOOST_CHECK(authorization.authorized_inputs[0]);
    BOOST_CHECK(!authorization.authorized_inputs[1]);

    // Replacing pool custody with an owner payout, changing the shard, adding
    // key material, or charging the vault for the fee all fail closed.
    CMutableTransaction redirected{spend};
    const auto escaped{modern::MakeAssetOwnerOutput(
        acceptance.asset, acceptance.amount, OwnerScript(0x44))};
    BOOST_REQUIRE(escaped.has_value());
    redirected.vout[0] = *escaped;
    BOOST_CHECK(!node::CheckFlowMeshVaultTransaction(
        CTransaction{redirected}, prevs, 1, C0 + 1, fixture.params, index,
        authorization, error));

    CMutableTransaction wrong_shard{spend};
    wrong_shard.vout[0] = *modern::MakeDexVaultOutput(
        acceptance.asset, acceptance.amount, acceptance.vault_id,
        modern::VAULT_KIND_POOL_CHANGE,
        static_cast<uint16_t>((acceptance.shard + 1) % 256));
    BOOST_CHECK(!node::CheckFlowMeshVaultTransaction(
        CTransaction{wrong_shard}, prevs, 1, C0 + 1, fixture.params, index,
        authorization, error));

    CMutableTransaction scripted{spend};
    scripted.vin[0].scriptSig << OP_TRUE;
    BOOST_CHECK(!node::CheckFlowMeshVaultTransaction(
        CTransaction{scripted}, prevs, 1, C0 + 1, fixture.params, index,
        authorization, error));
    CMutableTransaction witnessed{spend};
    witnessed.vin[0].scriptWitness.stack.push_back({0x01});
    std::string witness_error;
    BOOST_CHECK(!node::CheckFlowMeshVaultTransaction(
        CTransaction{witnessed}, prevs, 1, C0 + 1, fixture.params, index,
        authorization, witness_error));
    BOOST_CHECK_EQUAL(witness_error,
                      "DEX_VAULT input scriptSig and witness must be empty");

    CMutableTransaction fee_leak{spend};
    fee_leak.vout[1].nValue = 10;
    BOOST_CHECK(!node::CheckFlowMeshVaultTransaction(
        CTransaction{fee_leak}, prevs, 1, C0 + 1, fixture.params, index,
        authorization, error));

    // Without its owner input the sweep balances at a zero fee. The fee rule
    // alone would accept that, so the separate-input rule must refuse it.
    CMutableTransaction unfunded{spend};
    unfunded.vin.pop_back();
    unfunded.vout.pop_back();
    const std::vector<Coin> deposit_only{prevs.front()};
    BOOST_CHECK(!node::CheckFlowMeshVaultTransaction(
        CTransaction{unfunded}, deposit_only, 0, C0 + 1, fixture.params,
        index, authorization, error));
    BOOST_CHECK_EQUAL(
        error,
        "FlowMesh vault miner fee requires a separate native owner input");

    // A keyless custody spend carries its type-9 proof and no other record.
    CMutableTransaction bundled{spend};
    bundled.mpa.insert(bundled.mpa.begin(), fixture.CheckpointRecord(genesis));
    BOOST_CHECK(!node::CheckFlowMeshVaultTransaction(
        CTransaction{bundled}, prevs, 1, C0 + 1, fixture.params, index,
        authorization, error));
    BOOST_CHECK_EQUAL(
        error, "FlowMesh vault transaction must contain only its type-9 proof");
}

BOOST_AUTO_TEST_CASE(withdrawal_forces_destination_change_and_one_time_use)
{
    const Fixture fixture;
    node::FlowMeshCheckpointIndex index;
    const auto genesis{fixture.Genesis(A3)};
    const auto genesis_id{modern::FlowMeshCheckpointIdV1(genesis)};
    BOOST_REQUIRE(genesis_id.has_value());
    const CScript destination_script{OwnerScript(0x55)};
    modern::FlowMeshWithdrawalReceiptV1 receipt;
    receipt.receipt_id = Filled(0xf1);
    receipt.market_id = fixture.market;
    receipt.epoch = 0;
    receipt.sequence = 1;
    receipt.account = Filled(0xf2);
    receipt.asset = fixture.asset;
    receipt.amount = 40;
    receipt.destination_owner_commitment =
        modern::AssetOwnerCommitment(destination_script);
    receipt.vault_id = fixture.vault;
    receipt.deterministic_change_shard = 23;
    const std::vector<modern::FlowMeshEffectV1> effects{receipt};
    const auto core{fixture.Core(A3, 0, 1, *genesis_id, effects, 0)};
    const auto checkpoint_id{modern::FlowMeshCheckpointIdV1(core)};
    BOOST_REQUIRE(checkpoint_id.has_value());
    Connect(index, Verify(index, fixture,
                          Block({Tx({fixture.CheckpointRecord(genesis),
                                     fixture.CheckpointRecord(core)})}),
                          C0, static_cast<unsigned char>(C0 + 1)));

    modern::FlowMeshVaultProofV1 proof;
    proof.kind = modern::FlowMeshVaultProofKind::WITHDRAWAL;
    proof.checkpoint_id = *checkpoint_id;
    proof.effect = receipt;
    proof.leaf_index = 0;
    proof.branch = *modern::BuildFlowMeshEffectBranch(0, effects, 0);

    const auto pool_in{modern::MakeDexVaultOutput(
        fixture.asset, 100, fixture.vault,
        modern::VAULT_KIND_POOL_CHANGE, 7)};
    const auto payout{modern::MakeAssetOwnerOutput(
        fixture.asset, receipt.amount, destination_script)};
    const auto change{modern::MakeDexVaultOutput(
        fixture.asset, 60, fixture.vault,
        modern::VAULT_KIND_POOL_CHANGE,
        receipt.deterministic_change_shard)};
    BOOST_REQUIRE(pool_in.has_value());
    BOOST_REQUIRE(payout.has_value());
    BOOST_REQUIRE(change.has_value());

    const COutPoint pool_outpoint{Txid::FromUint256(Filled(0xf4)), 0};
    const COutPoint fee_outpoint{Txid::FromUint256(Filled(0xf5)), 0};
    CMutableTransaction spend;
    spend.version = 2;
    spend.vin.emplace_back(pool_outpoint);
    spend.vin.emplace_back(fee_outpoint);
    spend.vout = {*payout, *change, CTxOut{9, CScript() << OP_TRUE}};
    spend.mpa = {ProofRecord(proof)};
    const std::vector<Coin> prevs{
        Coin{*pool_in, A3, /*coinbase=*/false},
        Coin{CTxOut{10, CScript() << OP_TRUE}, A3,
             /*coinbase=*/false}};

    node::FlowMeshVaultAuthorization authorization;
    std::string error;
    BOOST_REQUIRE_MESSAGE(node::CheckFlowMeshVaultTransaction(
                              CTransaction{spend}, prevs, 1, C0 + 1,
                              fixture.params, index, authorization, error),
                          error);
    BOOST_CHECK(authorization.authorized_inputs[0]);
    BOOST_CHECK(!authorization.authorized_inputs[1]);

    CMutableTransaction redirected{spend};
    redirected.vout[0] = *modern::MakeAssetOwnerOutput(
        fixture.asset, receipt.amount, OwnerScript(0x56));
    BOOST_CHECK(!node::CheckFlowMeshVaultTransaction(
        CTransaction{redirected}, prevs, 1, C0 + 1, fixture.params, index,
        authorization, error));

    CMutableTransaction short_change{spend};
    short_change.vout[1] = *modern::MakeDexVaultOutput(
        fixture.asset, 59, fixture.vault,
        modern::VAULT_KIND_POOL_CHANGE,
        receipt.deterministic_change_shard);
    BOOST_CHECK(!node::CheckFlowMeshVaultTransaction(
        CTransaction{short_change}, prevs, 1, C0 + 1, fixture.params, index,
        authorization, error));

    // The change returns as one output, in the receipt's change shard.
    CMutableTransaction other_shard{spend};
    other_shard.vout[1] = *modern::MakeDexVaultOutput(
        fixture.asset, 60, fixture.vault, modern::VAULT_KIND_POOL_CHANGE,
        static_cast<uint16_t>(receipt.deterministic_change_shard + 1));
    std::string shard_error;
    BOOST_CHECK(!node::CheckFlowMeshVaultTransaction(
        CTransaction{other_shard}, prevs, 1, C0 + 1, fixture.params, index,
        authorization, shard_error));
    BOOST_CHECK_EQUAL(
        shard_error,
        "FlowMesh withdrawal has invalid or duplicate vault change");
    const auto half_change{modern::MakeDexVaultOutput(
        fixture.asset, 30, fixture.vault, modern::VAULT_KIND_POOL_CHANGE,
        receipt.deterministic_change_shard)};
    BOOST_REQUIRE(half_change.has_value());
    CMutableTransaction split_change{spend};
    split_change.vout[1] = *half_change;
    split_change.vout.insert(split_change.vout.begin() + 2, *half_change);
    std::string split_error;
    BOOST_CHECK(!node::CheckFlowMeshVaultTransaction(
        CTransaction{split_change}, prevs, 1, C0 + 1, fixture.params, index,
        authorization, split_error));
    BOOST_CHECK_EQUAL(
        split_error,
        "FlowMesh withdrawal has invalid or duplicate vault change");

    const auto user_deposit{modern::MakeDexVaultOutput(
        fixture.asset, 100, fixture.vault,
        modern::VAULT_KIND_USER_DEPOSIT,
        modern::FlowMeshUserDepositShard(fixture.vault, Filled(0xf6)),
        Filled(0xf6))};
    BOOST_REQUIRE(user_deposit.has_value());
    auto deposit_prevs{prevs};
    deposit_prevs[0] = Coin{*user_deposit, A3, /*coinbase=*/false};
    BOOST_CHECK(!node::CheckFlowMeshVaultTransaction(
        CTransaction{spend}, deposit_prevs, 1, C0 + 1, fixture.params, index,
        authorization, error));

    const auto proof_delta{Verify(index, fixture,
                                  Block({MakeTransactionRef(spend)}),
                                  C0 + 1,
                                  static_cast<unsigned char>(C0 + 2))};
    Connect(index, proof_delta);
    BOOST_REQUIRE_EQUAL(proof_delta.withdrawal_settlements.size(), 1U);
    BOOST_CHECK(proof_delta.withdrawal_settlements[0].receipt == receipt);
    std::vector<flowmesh::WithdrawalSettlementFactV1> settlements;
    BOOST_REQUIRE_MESSAGE(index.WithdrawalSettlementsBetween(
                              fixture.market, fixture.chain.indexes[C0],
                              fixture.chain.indexes[C0 + 1], settlements,
                              error),
                          error);
    BOOST_REQUIRE_EQUAL(settlements.size(), 1U);
    BOOST_CHECK(settlements[0] == proof_delta.withdrawal_settlements[0]);
    BOOST_REQUIRE(index.DisconnectBlock(
        C0 + 1, fixture.chain.hashes[C0 + 1], error));
    BOOST_CHECK(!index.IsNullified(proof_delta.nullifiers[0]));
    auto malformed_settlement{proof_delta};
    malformed_settlement.withdrawal_settlements[0].transaction_id.SetNull();
    BOOST_CHECK(!index.ConnectBlock(malformed_settlement, error));
    BOOST_CHECK(!index.IsNullified(proof_delta.nullifiers[0]));
    Connect(index, proof_delta);
    BOOST_CHECK(!node::CheckFlowMeshVaultTransaction(
        CTransaction{spend}, prevs, 1, C0 + 2, fixture.params, index,
        authorization, error));
    BOOST_CHECK_EQUAL(error, "FlowMesh vault effect is already nullified");
}

BOOST_AUTO_TEST_CASE(native_withdrawal_uses_exact_script_payout_and_owner_fee_change)
{
    const Fixture fixture;
    node::FlowMeshCheckpointIndex index;
    const auto genesis{fixture.Genesis(A3)};
    const auto genesis_id{modern::FlowMeshCheckpointIdV1(genesis)};
    BOOST_REQUIRE(genesis_id.has_value());
    const CScript destination_script{OwnerScript(0x58)};
    modern::FlowMeshWithdrawalReceiptV1 receipt;
    receipt.receipt_id = Filled(0x91);
    receipt.market_id = fixture.market;
    receipt.epoch = 0;
    receipt.sequence = 1;
    receipt.account = Filled(0x92);
    receipt.asset = modern::NativeAsset();
    receipt.amount = 40;
    receipt.destination_owner_commitment =
        modern::AssetOwnerCommitment(destination_script);
    receipt.vault_id = fixture.vault;
    receipt.deterministic_change_shard = 24;
    const std::vector<modern::FlowMeshEffectV1> effects{receipt};
    const auto core{fixture.Core(A3, 0, 1, *genesis_id, effects, 0)};
    const auto checkpoint_id{modern::FlowMeshCheckpointIdV1(core)};
    BOOST_REQUIRE(checkpoint_id.has_value());
    Connect(index, Verify(index, fixture,
                          Block({Tx({fixture.CheckpointRecord(genesis),
                                     fixture.CheckpointRecord(core)})}),
                          C0, 0x93));

    modern::FlowMeshVaultProofV1 proof;
    proof.kind = modern::FlowMeshVaultProofKind::WITHDRAWAL;
    proof.checkpoint_id = *checkpoint_id;
    proof.effect = receipt;
    proof.leaf_index = 0;
    proof.branch = *modern::BuildFlowMeshEffectBranch(0, effects, 0);

    const auto pool_in{modern::MakeDexVaultOutput(
        modern::NativeAsset(), 100, fixture.vault,
        modern::VAULT_KIND_POOL_CHANGE, 7)};
    const auto change{modern::MakeDexVaultOutput(
        modern::NativeAsset(), 60, fixture.vault,
        modern::VAULT_KIND_POOL_CHANGE,
        receipt.deterministic_change_shard)};
    BOOST_REQUIRE(pool_in.has_value());
    BOOST_REQUIRE(change.has_value());

    CMutableTransaction spend;
    spend.version = 2;
    spend.vin.emplace_back(
        COutPoint{Txid::FromUint256(Filled(0x94)), 0});
    spend.vin.emplace_back(
        COutPoint{Txid::FromUint256(Filled(0x95)), 0});
    spend.vout = {CTxOut{receipt.amount, destination_script}, *change,
                  CTxOut{9, OwnerScript(0x59)}};
    spend.mpa = {ProofRecord(proof)};
    const std::vector<Coin> prevs{
        Coin{*pool_in, A3, /*coinbase=*/false},
        Coin{CTxOut{10, OwnerScript(0x59)}, A3, /*coinbase=*/false}};

    node::FlowMeshVaultAuthorization authorization;
    std::string error;
    BOOST_REQUIRE_MESSAGE(node::CheckFlowMeshVaultTransaction(
                              CTransaction{spend}, prevs, 1, C0 + 1,
                              fixture.params, index, authorization, error),
                          error);
    BOOST_CHECK(authorization.authorized_inputs[0]);
    BOOST_CHECK(!authorization.authorized_inputs[1]);

    CMutableTransaction redirected{spend};
    redirected.vout[0] = CTxOut{receipt.amount, OwnerScript(0x5a)};
    BOOST_CHECK(!node::CheckFlowMeshVaultTransaction(
        CTransaction{redirected}, prevs, 1, C0 + 1, fixture.params, index,
        authorization, error));
}

BOOST_AUTO_TEST_CASE(replay_and_malformed_delta_application_are_exact)
{
    const Fixture fixture;
    const auto genesis{fixture.Genesis(A3)};
    const auto genesis_id{modern::FlowMeshCheckpointIdV1(genesis)};
    BOOST_REQUIRE(genesis_id.has_value());
    const modern::FlowMeshDepositAcceptanceV1 acceptance{fixture.Deposit(1)};
    const std::vector<modern::FlowMeshEffectV1> effects{acceptance};
    const auto core{fixture.Core(A3, 0, 1, *genesis_id, effects, 0)};
    const auto checkpoint_id{modern::FlowMeshCheckpointIdV1(core)};
    BOOST_REQUIRE(checkpoint_id.has_value());

    modern::FlowMeshVaultProofV1 proof;
    proof.kind = modern::FlowMeshVaultProofKind::DEPOSIT_SWEEP;
    proof.checkpoint_id = *checkpoint_id;
    proof.effect = acceptance;
    proof.leaf_index = 0;
    proof.branch = *modern::BuildFlowMeshEffectBranch(0, effects, 0);
    const CBlock checkpoint_block{
        Block({Tx({fixture.CheckpointRecord(genesis),
                   fixture.CheckpointRecord(core)})})};
    const CBlock proof_block{Block({Tx({ProofRecord(proof)})})};

    node::FlowMeshCheckpointIndex incremental;
    const auto checkpoint_delta{
        Verify(incremental, fixture, checkpoint_block, C0, 0xa8)};
    Connect(incremental, checkpoint_delta);
    const auto proof_delta{
        Verify(incremental, fixture, proof_block, C0 + 1, 0xa9)};
    Connect(incremental, proof_delta);

    // A clean reindex derives byte-identical history, heads and nullifiers.
    node::FlowMeshCheckpointIndex replay;
    Connect(replay,
            Verify(replay, fixture, checkpoint_block, C0, 0xa8));
    Connect(replay, Verify(replay, fixture, proof_block, C0 + 1, 0xa9));
    BOOST_CHECK(replay.History() == incremental.History());
    BOOST_CHECK(replay.Head(fixture.market) ==
                incremental.Head(fixture.market));
    BOOST_REQUIRE_EQUAL(proof_delta.nullifiers.size(), 1U);
    BOOST_CHECK(replay.IsNullified(proof_delta.nullifiers[0]));

    std::string error;
    BOOST_REQUIRE(
        incremental.DisconnectBlock(C0 + 1, Filled(0xa9), error));
    BOOST_REQUIRE(
        incremental.DisconnectBlock(C0, Filled(0xa8), error));
    BOOST_CHECK(incremental.History().empty());
    BOOST_CHECK(!incremental.Head(fixture.market));
    BOOST_CHECK(!incremental.IsNullified(proof_delta.nullifiers[0]));

    // ConnectBlock is atomic and independently binds every stored object to
    // the enclosing delta, rather than trusting a caller-mutated preview.
    node::FlowMeshCheckpointIndex guarded;
    auto wrong_block{checkpoint_delta};
    wrong_block.checkpoints[0].connected_block = Filled(0xaa);
    BOOST_CHECK(!guarded.ConnectBlock(wrong_block, error));
    BOOST_CHECK_EQUAL(guarded.CheckpointCount(), 0U);
    BOOST_CHECK(guarded.History().empty());

    auto wrong_id{checkpoint_delta};
    wrong_id.checkpoints[0].checkpoint_id = Filled(0xab);
    BOOST_CHECK(!guarded.ConnectBlock(wrong_id, error));
    BOOST_CHECK_EQUAL(guarded.CheckpointCount(), 0U);

    Connect(guarded, checkpoint_delta);
    auto null_effect{proof_delta};
    null_effect.nullifiers[0].effect_id.SetNull();
    BOOST_CHECK(!guarded.ConnectBlock(null_effect, error));
    BOOST_CHECK_EQUAL(guarded.NullifierCount(), 0U);

    auto unknown_nullifier{proof_delta};
    unknown_nullifier.nullifiers[0].kind =
        static_cast<node::FlowMeshNullifierKind>(0xff);
    BOOST_CHECK(!guarded.ConnectBlock(unknown_nullifier, error));
    BOOST_CHECK_EQUAL(guarded.NullifierCount(), 0U);
    BOOST_REQUIRE(guarded.Head(fixture.market).has_value());
    BOOST_CHECK(guarded.Head(fixture.market)->checkpoint_id == *checkpoint_id);
}

BOOST_AUTO_TEST_CASE(withdrawal_stays_inside_its_markets_vault)
{
    // Every market quotes native B3, so every market's vault holds native
    // pool change. Only the receipt's vault id keeps one market's withdrawal
    // off another market's coins and its change out of another market's
    // vault; each spend below is otherwise exact.
    const Fixture fixture;
    node::FlowMeshCheckpointIndex index;
    const auto genesis{fixture.Genesis(A3)};
    const auto genesis_id{modern::FlowMeshCheckpointIdV1(genesis)};
    BOOST_REQUIRE(genesis_id.has_value());
    const CScript destination_script{OwnerScript(0x61)};
    modern::FlowMeshWithdrawalReceiptV1 receipt;
    receipt.receipt_id = Filled(0x62);
    receipt.market_id = fixture.market;
    receipt.epoch = 0;
    receipt.sequence = 1;
    receipt.account = Filled(0x63);
    receipt.asset = modern::NativeAsset();
    receipt.amount = 40;
    receipt.destination_owner_commitment =
        modern::AssetOwnerCommitment(destination_script);
    receipt.vault_id = fixture.vault;
    receipt.deterministic_change_shard = 25;
    const std::vector<modern::FlowMeshEffectV1> effects{receipt};
    const auto core{fixture.Core(A3, 0, 1, *genesis_id, effects, 0)};
    const auto checkpoint_id{modern::FlowMeshCheckpointIdV1(core)};
    BOOST_REQUIRE(checkpoint_id.has_value());
    Connect(index, Verify(index, fixture,
                          Block({Tx({fixture.CheckpointRecord(genesis),
                                     fixture.CheckpointRecord(core)})}),
                          C0, 0x64));

    modern::FlowMeshVaultProofV1 proof;
    proof.kind = modern::FlowMeshVaultProofKind::WITHDRAWAL;
    proof.checkpoint_id = *checkpoint_id;
    proof.effect = receipt;
    proof.leaf_index = 0;
    proof.branch = *modern::BuildFlowMeshEffectBranch(0, effects, 0);

    const auto other_market{
        flowmesh::ComputeFlowMeshMarketId(fixture.domain, Filled(0x36))};
    BOOST_REQUIRE(other_market.has_value());
    const auto other_vault{
        flowmesh::ComputeFlowMeshVaultId(fixture.domain, *other_market)};
    BOOST_REQUIRE(other_vault.has_value());
    BOOST_REQUIRE(*other_vault != fixture.vault);
    const auto own_pool{modern::MakeDexVaultOutput(
        modern::NativeAsset(), 100, fixture.vault,
        modern::VAULT_KIND_POOL_CHANGE, 7)};
    const auto other_pool{modern::MakeDexVaultOutput(
        modern::NativeAsset(), 100, *other_vault,
        modern::VAULT_KIND_POOL_CHANGE, 7)};
    const auto change{modern::MakeDexVaultOutput(
        modern::NativeAsset(), 60, fixture.vault,
        modern::VAULT_KIND_POOL_CHANGE,
        receipt.deterministic_change_shard)};
    const auto two_coin_change{modern::MakeDexVaultOutput(
        modern::NativeAsset(), 160, fixture.vault,
        modern::VAULT_KIND_POOL_CHANGE,
        receipt.deterministic_change_shard)};
    BOOST_REQUIRE(own_pool.has_value());
    BOOST_REQUIRE(other_pool.has_value());
    BOOST_REQUIRE(change.has_value());
    BOOST_REQUIRE(two_coin_change.has_value());
    const Coin own{*own_pool, A3, /*fCoinBaseIn=*/false};
    const Coin other{*other_pool, A3, /*fCoinBaseIn=*/false};
    const Coin fee{CTxOut{10, OwnerScript(0x67)}, A3, /*fCoinBaseIn=*/false};

    CMutableTransaction spend;
    spend.version = 2;
    spend.vin.emplace_back(COutPoint{Txid::FromUint256(Filled(0x65)), 0});
    spend.vin.emplace_back(COutPoint{Txid::FromUint256(Filled(0x66)), 0});
    spend.vout = {CTxOut{receipt.amount, destination_script}, *change,
                  CTxOut{9, OwnerScript(0x67)}};
    spend.mpa = {ProofRecord(proof)};

    node::FlowMeshVaultAuthorization authorization;
    std::string error;
    BOOST_REQUIRE_MESSAGE(node::CheckFlowMeshVaultTransaction(
                              CTransaction{spend}, {own, fee}, 1, C0 + 1,
                              fixture.params, index, authorization, error),
                          error);
    BOOST_CHECK(!node::CheckFlowMeshVaultTransaction(
        CTransaction{spend}, {other, fee}, 1, C0 + 1, fixture.params, index,
        authorization, error));
    BOOST_CHECK_EQUAL(error,
                      "FlowMesh withdrawal input is not matching pool change");

    // Beside a coin of the receipt's vault, with all of it returned there as
    // change, the other market's coin is still refused. Two coins of the
    // receipt's own vault pass.
    CMutableTransaction two_coins{spend};
    two_coins.vin.insert(two_coins.vin.begin() + 1,
                         CTxIn{COutPoint{Txid::FromUint256(Filled(0x68)), 0}});
    two_coins.vout[1] = *two_coin_change;
    BOOST_CHECK(!node::CheckFlowMeshVaultTransaction(
        CTransaction{two_coins}, {own, other, fee}, 1, C0 + 1, fixture.params,
        index, authorization, error));
    BOOST_CHECK_EQUAL(error,
                      "FlowMesh withdrawal input is not matching pool change");
    BOOST_CHECK_MESSAGE(node::CheckFlowMeshVaultTransaction(
                            CTransaction{two_coins}, {own, own, fee}, 1,
                            C0 + 1, fixture.params, index, authorization,
                            error),
                        error);

    // The receipt's own coin cannot return its change to the other vault.
    const auto other_change{modern::MakeDexVaultOutput(
        modern::NativeAsset(), 60, *other_vault,
        modern::VAULT_KIND_POOL_CHANGE,
        receipt.deterministic_change_shard)};
    BOOST_REQUIRE(other_change.has_value());
    CMutableTransaction moved_change{spend};
    moved_change.vout[1] = *other_change;
    BOOST_CHECK(!node::CheckFlowMeshVaultTransaction(
        CTransaction{moved_change}, {own, fee}, 1, C0 + 1, fixture.params,
        index, authorization, error));
    BOOST_CHECK_EQUAL(
        error, "FlowMesh withdrawal has invalid or duplicate vault change");
}

BOOST_AUTO_TEST_CASE(deposit_sweep_moves_only_the_accepted_coin_to_its_vault)
{
    // A second deposit coin with the same account, asset, amount, vault and
    // shard is still another deposit. Vault inputs are keyless, so anyone
    // could otherwise sweep it with this acceptance and leave the accepted
    // coin behind with its acceptance used up. The swept coin must also stay
    // in the acceptance's vault, not move to another market's pool.
    const Fixture fixture;
    node::FlowMeshCheckpointIndex index;
    const auto genesis{fixture.Genesis(A3)};
    const auto genesis_id{modern::FlowMeshCheckpointIdV1(genesis)};
    BOOST_REQUIRE(genesis_id.has_value());
    const modern::FlowMeshDepositAcceptanceV1 acceptance{fixture.Deposit(1)};
    const std::vector<modern::FlowMeshEffectV1> effects{acceptance};
    const auto core{fixture.Core(A3, 0, 1, *genesis_id, effects, 0)};
    const auto checkpoint_id{modern::FlowMeshCheckpointIdV1(core)};
    BOOST_REQUIRE(checkpoint_id.has_value());
    Connect(index, Verify(index, fixture,
                          Block({Tx({fixture.CheckpointRecord(genesis),
                                     fixture.CheckpointRecord(core)})}),
                          C0, 0x69));

    modern::FlowMeshVaultProofV1 proof;
    proof.kind = modern::FlowMeshVaultProofKind::DEPOSIT_SWEEP;
    proof.checkpoint_id = *checkpoint_id;
    proof.effect = acceptance;
    proof.leaf_index = 0;
    proof.branch = *modern::BuildFlowMeshEffectBranch(0, effects, 0);

    const auto deposit{modern::MakeDexVaultOutput(
        acceptance.asset, acceptance.amount, acceptance.vault_id,
        modern::VAULT_KIND_USER_DEPOSIT, acceptance.shard,
        acceptance.account)};
    const auto pool{modern::MakeDexVaultOutput(
        acceptance.asset, acceptance.amount, acceptance.vault_id,
        modern::VAULT_KIND_POOL_CHANGE, acceptance.shard)};
    BOOST_REQUIRE(deposit.has_value());
    BOOST_REQUIRE(pool.has_value());

    CMutableTransaction spend;
    spend.version = 2;
    spend.vin.emplace_back(acceptance.deposit_outpoint);
    spend.vin.emplace_back(COutPoint{Txid::FromUint256(Filled(0x6a)), 0});
    spend.vout = {*pool, CTxOut{9, CScript() << OP_TRUE}};
    spend.mpa = {ProofRecord(proof)};
    const std::vector<Coin> prevs{
        Coin{*deposit, A3, /*fCoinBaseIn=*/false},
        Coin{CTxOut{10, CScript() << OP_TRUE}, A3, /*fCoinBaseIn=*/false}};

    node::FlowMeshVaultAuthorization authorization;
    std::string error;
    BOOST_REQUIRE_MESSAGE(node::CheckFlowMeshVaultTransaction(
                              CTransaction{spend}, prevs, 1, C0 + 1,
                              fixture.params, index, authorization, error),
                          error);

    CMutableTransaction twin{spend};
    twin.vin[0].prevout = COutPoint{acceptance.deposit_outpoint.hash,
                                    acceptance.deposit_outpoint.n + 1};
    BOOST_CHECK(!node::CheckFlowMeshVaultTransaction(
        CTransaction{twin}, prevs, 1, C0 + 1, fixture.params, index,
        authorization, error));
    BOOST_CHECK_EQUAL(
        error, "FlowMesh deposit sweep input does not match its acceptance");

    const auto other_market{
        flowmesh::ComputeFlowMeshMarketId(fixture.domain, Filled(0x36))};
    BOOST_REQUIRE(other_market.has_value());
    const auto other_vault{
        flowmesh::ComputeFlowMeshVaultId(fixture.domain, *other_market)};
    BOOST_REQUIRE(other_vault.has_value());
    BOOST_REQUIRE(*other_vault != acceptance.vault_id);
    const auto other_pool{modern::MakeDexVaultOutput(
        acceptance.asset, acceptance.amount, *other_vault,
        modern::VAULT_KIND_POOL_CHANGE, acceptance.shard)};
    BOOST_REQUIRE(other_pool.has_value());
    CMutableTransaction moved{spend};
    moved.vout[0] = *other_pool;
    BOOST_CHECK(!node::CheckFlowMeshVaultTransaction(
        CTransaction{moved}, prevs, 1, C0 + 1, fixture.params, index,
        authorization, error));
    BOOST_CHECK_EQUAL(
        error,
        "FlowMesh deposit sweep does not recreate the exact pool output");
}

BOOST_AUTO_TEST_CASE(deposit_sweep_refuses_acceptances_misstating_their_coin)
{
    // A checkpoint connects on its seats' signatures; nothing compares its
    // deposit acceptances with the chain's deposit coins. The sweep's input
    // check alone binds an acceptance to the coin it moves. Each acceptance
    // below names a real deposit coin but misstates one fact of it, and the
    // sweep it would authorize is otherwise exact.
    const Fixture fixture;
    node::FlowMeshCheckpointIndex index;
    const auto genesis{fixture.Genesis(A3)};
    const auto genesis_id{modern::FlowMeshCheckpointIdV1(genesis)};
    BOOST_REQUIRE(genesis_id.has_value());
    const auto other_market{
        flowmesh::ComputeFlowMeshMarketId(fixture.domain, Filled(0x36))};
    BOOST_REQUIRE(other_market.has_value());
    const auto other_vault{
        flowmesh::ComputeFlowMeshVaultId(fixture.domain, *other_market)};
    BOOST_REQUIRE(other_vault.has_value());
    const uint256 other_account{Filled(0x78)};

    // The coin is in another market's vault.
    modern::FlowMeshDepositAcceptanceV1 vault_lie{fixture.Deposit(1)};
    vault_lie.shard =
        modern::FlowMeshUserDepositShard(*other_vault, vault_lie.account);
    // The coin belongs to another account.
    modern::FlowMeshDepositAcceptanceV1 account_lie{fixture.Deposit(1)};
    account_lie.acceptance_id = Filled(0x79);
    account_lie.deposit_outpoint =
        COutPoint{Txid::FromUint256(Filled(0x7a)), 0};
    account_lie.shard =
        modern::FlowMeshUserDepositShard(account_lie.vault_id, other_account);
    // The coin is in another shard.
    modern::FlowMeshDepositAcceptanceV1 shard_lie{fixture.Deposit(1)};
    shard_lie.acceptance_id = Filled(0x7b);
    shard_lie.deposit_outpoint =
        COutPoint{Txid::FromUint256(Filled(0x7c)), 0};
    shard_lie.shard = static_cast<uint16_t>((shard_lie.shard + 1) % 256);
    const std::vector<modern::FlowMeshEffectV1> effects{vault_lie, account_lie,
                                                        shard_lie};
    const auto core{fixture.Core(A3, 0, 1, *genesis_id, effects, 0)};
    const auto checkpoint_id{modern::FlowMeshCheckpointIdV1(core)};
    BOOST_REQUIRE(checkpoint_id.has_value());
    Connect(index, Verify(index, fixture,
                          Block({Tx({fixture.CheckpointRecord(genesis),
                                     fixture.CheckpointRecord(core)})}),
                          C0, 0x7d));

    // Sweep the coin of `coin_account` in `coin_vault` (in its deterministic
    // shard) at the outpoint `lie` names, into the pool output `lie` names.
    const std::string mismatch{
        "FlowMesh deposit sweep input does not match its acceptance"};
    const auto refused = [&](const modern::FlowMeshDepositAcceptanceV1& lie,
                             const uint32_t leaf, const uint256& coin_vault,
                             const uint256& coin_account) {
        modern::FlowMeshVaultProofV1 proof;
        proof.kind = modern::FlowMeshVaultProofKind::DEPOSIT_SWEEP;
        proof.checkpoint_id = *checkpoint_id;
        proof.effect = lie;
        proof.leaf_index = leaf;
        proof.branch = *modern::BuildFlowMeshEffectBranch(0, effects, leaf);
        const auto coin{modern::MakeDexVaultOutput(
            lie.asset, lie.amount, coin_vault,
            modern::VAULT_KIND_USER_DEPOSIT,
            modern::FlowMeshUserDepositShard(coin_vault, coin_account),
            coin_account)};
        const auto pool{modern::MakeDexVaultOutput(
            lie.asset, lie.amount, lie.vault_id,
            modern::VAULT_KIND_POOL_CHANGE, lie.shard)};
        BOOST_REQUIRE(coin.has_value());
        BOOST_REQUIRE(pool.has_value());
        CMutableTransaction spend;
        spend.version = 2;
        spend.vin.emplace_back(lie.deposit_outpoint);
        spend.vin.emplace_back(COutPoint{Txid::FromUint256(Filled(0x7e)), 0});
        spend.vout = {*pool, CTxOut{9, CScript() << OP_TRUE}};
        spend.mpa = {ProofRecord(proof)};
        const std::vector<Coin> prevs{
            Coin{*coin, A3, /*fCoinBaseIn=*/false},
            Coin{CTxOut{10, CScript() << OP_TRUE}, A3, /*fCoinBaseIn=*/false}};
        node::FlowMeshVaultAuthorization authorization;
        std::string error;
        const bool allowed{node::CheckFlowMeshVaultTransaction(
            CTransaction{spend}, prevs, 1, C0 + 1, fixture.params, index,
            authorization, error)};
        return !allowed && error == mismatch;
    };
    BOOST_CHECK_MESSAGE(refused(vault_lie, 0, *other_vault, vault_lie.account),
                        "an acceptance misstating the coin's vault");
    BOOST_CHECK_MESSAGE(refused(account_lie, 1, account_lie.vault_id,
                                other_account),
                        "an acceptance misstating the coin's account");
    BOOST_CHECK_MESSAGE(refused(shard_lie, 2, shard_lie.vault_id,
                                shard_lie.account),
                        "an acceptance misstating the coin's shard");
}

BOOST_AUTO_TEST_CASE(vault_proof_effect_must_be_in_its_checkpoint)
{
    // A vault effect counts only if its checkpoint's seats signed it. A copy
    // of a signed receipt with another destination still names a connected
    // checkpoint, its leaf index and its branch, and the spend below pays
    // that destination exactly. The transaction rule and the block rule
    // must each refuse it on their own.
    const Fixture fixture;
    node::FlowMeshCheckpointIndex index;
    const auto genesis{fixture.Genesis(A3)};
    const auto genesis_id{modern::FlowMeshCheckpointIdV1(genesis)};
    BOOST_REQUIRE(genesis_id.has_value());
    const CScript destination_script{OwnerScript(0x6b)};
    modern::FlowMeshWithdrawalReceiptV1 receipt;
    receipt.receipt_id = Filled(0x6c);
    receipt.market_id = fixture.market;
    receipt.epoch = 0;
    receipt.sequence = 1;
    receipt.account = Filled(0x6d);
    receipt.asset = modern::NativeAsset();
    receipt.amount = 40;
    receipt.destination_owner_commitment =
        modern::AssetOwnerCommitment(destination_script);
    receipt.vault_id = fixture.vault;
    receipt.deterministic_change_shard = 26;
    const std::vector<modern::FlowMeshEffectV1> effects{receipt};
    const auto core{fixture.Core(A3, 0, 1, *genesis_id, effects, 0)};
    const auto checkpoint_id{modern::FlowMeshCheckpointIdV1(core)};
    BOOST_REQUIRE(checkpoint_id.has_value());
    Connect(index, Verify(index, fixture,
                          Block({Tx({fixture.CheckpointRecord(genesis),
                                     fixture.CheckpointRecord(core)})}),
                          C0, 0x6e));

    modern::FlowMeshVaultProofV1 proof;
    proof.kind = modern::FlowMeshVaultProofKind::WITHDRAWAL;
    proof.checkpoint_id = *checkpoint_id;
    proof.effect = receipt;
    proof.leaf_index = 0;
    proof.branch = *modern::BuildFlowMeshEffectBranch(0, effects, 0);

    const auto pool_in{modern::MakeDexVaultOutput(
        modern::NativeAsset(), 100, fixture.vault,
        modern::VAULT_KIND_POOL_CHANGE, 7)};
    const auto change{modern::MakeDexVaultOutput(
        modern::NativeAsset(), 60, fixture.vault,
        modern::VAULT_KIND_POOL_CHANGE,
        receipt.deterministic_change_shard)};
    BOOST_REQUIRE(pool_in.has_value());
    BOOST_REQUIRE(change.has_value());
    const std::vector<Coin> prevs{
        Coin{*pool_in, A3, /*fCoinBaseIn=*/false},
        Coin{CTxOut{10, OwnerScript(0x6f)}, A3, /*fCoinBaseIn=*/false}};

    CMutableTransaction spend;
    spend.version = 2;
    spend.vin.emplace_back(COutPoint{Txid::FromUint256(Filled(0x70)), 0});
    spend.vin.emplace_back(COutPoint{Txid::FromUint256(Filled(0x74)), 0});
    spend.vout = {CTxOut{receipt.amount, destination_script}, *change,
                  CTxOut{9, OwnerScript(0x6f)}};
    spend.mpa = {ProofRecord(proof)};

    node::FlowMeshVaultAuthorization authorization;
    std::string error;
    BOOST_REQUIRE_MESSAGE(node::CheckFlowMeshVaultTransaction(
                              CTransaction{spend}, prevs, 1, C0 + 1,
                              fixture.params, index, authorization, error),
                          error);
    Verify(index, fixture, Block({MakeTransactionRef(spend)}), C0 + 1, 0x75);

    const CScript thief_script{OwnerScript(0x76)};
    modern::FlowMeshWithdrawalReceiptV1 forged_receipt{receipt};
    forged_receipt.destination_owner_commitment =
        modern::AssetOwnerCommitment(thief_script);
    modern::FlowMeshVaultProofV1 forged_proof{proof};
    forged_proof.effect = forged_receipt;
    CMutableTransaction forged{spend};
    forged.vout[0] = CTxOut{receipt.amount, thief_script};
    forged.mpa = {ProofRecord(forged_proof)};
    BOOST_CHECK(!node::CheckFlowMeshVaultTransaction(
        CTransaction{forged}, prevs, 1, C0 + 1, fixture.params, index,
        authorization, error));
    BOOST_CHECK_EQUAL(error,
                      "FlowMesh vault proof is not included in its checkpoint");
    node::FlowMeshCheckpointBlockDelta delta;
    std::string block_error;
    BOOST_CHECK(!index.VerifyBlock(Block({MakeTransactionRef(forged)}), C0 + 1,
                                   Filled(0x77), fixture.chain.chain,
                                   fixture.params, fixture.seats,
                                   fixture.vaults, delta, block_error));
    BOOST_CHECK_EQUAL(block_error,
                      "FlowMesh vault proof is not included in its checkpoint");
}

BOOST_AUTO_TEST_SUITE_END()
