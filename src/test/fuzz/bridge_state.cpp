// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! The bridge's consensus state end to end: node::BridgeStateIndex verifying,
//! connecting and disconnecting B3 blocks that mint bUSD against Ethereum
//! deposits and burn it back, under the mainnet bridge parameters.
//!
//! The world is a short synthetic Ethereum chain whose blocks hold real
//! receipt tries. Beside the genuine vault deposits sit lookalikes: events of
//! another contract or token, malformed Deposit logs, reverted receipts,
//! receipts of a type the V1 rules cannot read, and a side fork reusing
//! deposit ids. Candidate blocks carry honest records, records with a defect
//! and arbitrary bytes. A model of the active chain then checks:
//!  - on every block VerifyBlock accepts, that each mint pays one genuine
//!    canonical deposit, once, exactly its amount to exactly its recipient,
//!    within the block and epoch caps, under a fresh light-client head inside
//!    the pinned fork schedule; that each new anchor is a canonical block;
//!    and that each burn names its exact burn output and takes the next
//!    withdrawal id;
//!  - on blocks of honest records, the model's own verdict and delta, so
//!    clean mints, backfills and burns are accepted exactly as expected and
//!    the state's refusals (spent deposits, caps, stale heads) hold; and that
//!    a record with a defect every node refuses is refused;
//!  - that verifying changes nothing, that ConnectBlock takes whatever
//!    VerifyBlock accepted, and that the index then answers as the model;
//!  - that disconnecting restores the exact prior state, and reconnecting
//!    reproduces the same delta;
//!  - that VerifyTransaction and incremental block preview agree with
//!    VerifyBlock;
//!  - that a fresh index replaying the active chain ends in the same state.
//! The light client is seeded and advanced by hand-built deltas, as accepted
//! BOOTSTRAP and UPDATE records would change it; their proofs have their own
//! target (bridge_light_client). Mainnet has no bridge V2 envelope, so the V2
//! paths stay unreached here.

#include <node/bridge_state.h>

#include <bridge/admission.h>
#include <bridge/deposit.h>
#include <bridge/eth_light_client.h>
#include <bridge/proof.h>
#include <bridge/rlp.h>
#include <chainparams.h>
#include <consensus/amount.h>
#include <consensus/bridge_params.h>
#include <consensus/era.h>
#include <consensus/params.h>
#include <crypto/common.h>
#include <crypto/keccak256.h>
#include <modern/asset_output.h>
#include <modern/bridge_asset.h>
#include <modern/bridge_binding.h>
#include <modern/withdrawal_tree.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/util/bridge_eth_trie.h>
#include <uint256.h>
#include <util/chaintype.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using Bytes = std::vector<unsigned char>;
using Word = std::array<unsigned char, 32>;

//! At most this many steps (a candidate block, a light-client advance, a
//! disconnect or a reconnect) per input.
constexpr unsigned MAX_STEPS{24};
//! The first execution block's timestamp; each block adds 12 seconds.
constexpr uint64_t FIRST_TIMESTAMP{1'760'000'000};
//! The highest EIP-2718 receipt type the V1 rules read: the model's own
//! copy, so a change to bridge::RECEIPT_MAX_TYPE_V1 shows.
constexpr uint8_t V1_MAX_RECEIPT_TYPE{0x04};

const Consensus::Params* g_params{nullptr};
modern::AssetId g_asset{};
uint256 g_registry{};

const Consensus::BridgeAssetParams& Bridge() { return *g_params->busd_bridge; }
const Consensus::EthereumLightClientPins& Pins() { return *Bridge().light_client; }
const Consensus::BridgeMintCaps& Caps() { return *Bridge().mint_caps; }
int Activation() { return *Bridge().activation_height; }

//! The mint epoch of a height at or above activation.
uint64_t Epoch(const int height)
{
    return static_cast<uint64_t>(height - Activation()) / Caps().epoch_length_blocks;
}

void initialize_bridge_state()
{
    SelectParams(ChainType::MAIN);
    g_params = &Params().GetConsensus();
    const auto asset{modern::ConfiguredBridgeAssetId(*g_params)};
    const auto registry{modern::ConfiguredBridgeRegistryId(*g_params)};
    assert(asset && registry);
    g_asset = *asset;
    g_registry = *registry;
    // The model relies on these mainnet pins: bUSD keeps USDT's decimals,
    // decentralized burns open with mints, and bridge V2 never starts.
    assert(Bridge().asset.origin_decimals == Bridge().asset.asset_decimals);
    assert(*Bridge().withdrawal_mode == Consensus::BridgeWithdrawalMode::DECENTRALIZED_VERIFIER_V1);
    assert(Consensus::BridgeWithdrawalRulesActive(Activation(), *g_params));
    assert(!g_params->BridgeV2RulesActive(Activation() + 3 * static_cast<int>(Caps().epoch_length_blocks)));
}

bool IsZero(const unsigned char byte) { return byte == 0; }

//! The script a Deposit's recipient word pays, decoded here independently of
//! bridge::DecodeRecipientV1: eleven zero bytes, the P2PKH version byte, and
//! a 20-byte key hash that is not all zero.
std::optional<CScript> ReferenceRecipient(const Word& word)
{
    if (!std::all_of(word.begin(), word.begin() + 11, IsZero) ||
        word[11] != Consensus::BRIDGE_RECIPIENT_VERSION_P2PKH_V1 ||
        std::all_of(word.begin() + 12, word.end(), IsZero)) {
        return std::nullopt;
    }
    return CScript{} << OP_DUP << OP_HASH160 << Bytes(word.begin() + 12, word.end()) << OP_EQUALVERIFY
                     << OP_CHECKSIG;
}

//! A Deposit's amount word in bUSD units. bUSD keeps USDT's decimals, so it
//! is the 256-bit value itself, when positive and at most MAX_MONEY.
std::optional<CAmount> ReferenceAmount(const Word& word)
{
    if (!std::all_of(word.begin(), word.begin() + 24, IsZero)) return std::nullopt;
    const uint64_t value{ReadBE64(word.data() + 24)};
    if (value == 0 || value > static_cast<uint64_t>(MAX_MONEY)) return std::nullopt;
    return static_cast<CAmount>(value);
}

//! What a lenient reading of a Deposit's words would pay, for a deposit no
//! node mints: the low 64 bits of its amount (else one unit), and its last
//! 20 bytes as a P2PKH key hash. A mint record of such a deposit pays this,
//! so a rule that let the deposit through would find its output.
CAmount LenientAmount(const Word& word)
{
    const uint64_t value{ReadBE64(word.data() + 24)};
    return value == 0 || value > static_cast<uint64_t>(MAX_MONEY) ? 1 : static_cast<CAmount>(value);
}

CScript LenientRecipient(const Word& word)
{
    return CScript{} << OP_DUP << OP_HASH160 << Bytes(word.begin() + 12, word.end()) << OP_EQUALVERIFY
                     << OP_CHECKSIG;
}

//! Whether a block at `time` may mint under a head finalized at `finalized`:
//! at most max_sync_lag_slots slots of 12 seconds behind it.
bool Fresh(const uint32_t time, const uint64_t finalized)
{
    return time <= finalized || time - finalized <= Pins().max_sync_lag_slots * 12;
}

bridge::BridgeDepositKey Key(const uint64_t deposit_id)
{
    return {Bridge().asset.origin_chain_id, Bridge().asset.vault_address, deposit_id,
            bridge::BRIDGE_NULLIFIER_NAMESPACE_V1};
}

CScript P2PKH(const unsigned char fill)
{
    return CScript{} << OP_DUP << OP_HASH160 << Bytes(20, fill) << OP_EQUALVERIFY << OP_CHECKSIG;
}

template <typename T>
const T& Pick(FuzzedDataProvider& fdp, const std::vector<T>& values)
{
    return values[fdp.ConsumeIntegralInRange<size_t>(0, values.size() - 1)];
}

uint256 Keccak(const std::span<const unsigned char> bytes)
{
    uint256 out;
    Keccak256().Write(bytes).Finalize(out);
    return out;
}

Word UintWord(const uint64_t value)
{
    Word out{};
    WriteBE64(out.data() + 24, value);
    return out;
}

Word AddressWord(const bridge::EthAddress& address)
{
    Word out{};
    std::copy(address.begin(), address.end(), out.begin() + 12);
    return out;
}

Bytes EncodeLog(const bridge::EthAddress& emitter, const std::vector<Word>& topics, const Bytes& data)
{
    std::vector<Bytes> encoded;
    encoded.reserve(topics.size());
    for (const Word& topic : topics) encoded.push_back(bridge::RlpEncodeBytes(topic));
    return bridge::RlpEncodeList(
        {bridge::RlpEncodeBytes(emitter), bridge::RlpEncodeList(encoded), bridge::RlpEncodeBytes(data)});
}

//! A post-London header: its parent, receipts root, number and timestamp,
//! every other field empty.
Bytes ExecutionHeader(const uint256& parent, const uint64_t number, const uint256& receipts_root,
                      const uint64_t timestamp)
{
    std::vector<Bytes> fields(16, bridge::RlpEncodeBytes(std::span<const unsigned char>{}));
    fields[0] = bridge::RlpEncodeBytes(std::span<const unsigned char>{parent.begin(), 32});
    fields[5] = bridge::RlpEncodeBytes(std::span<const unsigned char>{receipts_root.begin(), 32});
    fields[8] = bridge::RlpEncodeUint64(number);
    fields[11] = bridge::RlpEncodeUint64(timestamp);
    return bridge::RlpEncodeList(fields);
}

//! A Deposit amount word: small, around a third, half or all of the block
//! cap, around MAX_MONEY, zero or any 64-bit value; at times wider.
Word ConsumeAmount(FuzzedDataProvider& fdp)
{
    const uint64_t cap{static_cast<uint64_t>(Caps().max_per_block)};
    const uint64_t near{fdp.ConsumeIntegralInRange<uint64_t>(0, 2)};
    uint64_t value{0};
    switch (fdp.ConsumeIntegralInRange<int>(0, 6)) {
    case 0: value = fdp.ConsumeIntegralInRange<uint64_t>(1, 1'000'000'000); break;
    case 1: value = cap / 3 + near; break;
    case 2: value = cap / 2 + near - 1; break;
    case 3: value = cap + near - 1; break;
    case 4: value = static_cast<uint64_t>(MAX_MONEY) + near - 1; break;
    case 5: value = 0; break;
    default: value = fdp.ConsumeIntegral<uint64_t>(); break;
    }
    Word word{UintWord(value)};
    if (fdp.ConsumeIntegralInRange<int>(0, 15) == 15) word[fdp.ConsumeIntegralInRange<size_t>(0, 23)] = 1;
    return word;
}

//! A Deposit recipient word: mostly the P2PKH encoding of one of a few key
//! hashes, at times with a padding byte set, another version byte or the
//! all-zero hash.
Word ConsumeRecipient(FuzzedDataProvider& fdp)
{
    Word word{};
    word[11] = Consensus::BRIDGE_RECIPIENT_VERSION_P2PKH_V1;
    std::fill(word.begin() + 12, word.end(), fdp.ConsumeIntegralInRange<unsigned char>(1, 4));
    switch (fdp.ConsumeIntegralInRange<int>(0, 9)) {
    case 7: word[fdp.ConsumeIntegralInRange<size_t>(0, 10)] = 1; break;
    case 8: word[11] = fdp.ConsumeBool() ? 0 : fdp.ConsumeIntegralInRange<unsigned char>(2, 255); break;
    case 9: std::fill(word.begin() + 12, word.end(), 0); break;
    default: break;
    }
    return word;
}

//! One Deposit-shaped log of the world: a genuine vault deposit or a
//! lookalike.
struct Deposit {
    uint64_t id{0};
    Word amount{};
    Word recipient{};
    size_t block{0};
    uint64_t tx_index{0};
    uint32_t log_index{0};
    //! A Deposit the vault really emitted: well formed, of the configured
    //! token, in a successful receipt of a canonical block.
    bool genuine{false};
    //! Genuine, in a receipt of an EIP-2718 type the V1 rules can read.
    bool provable{false};
};

struct ExecBlock {
    uint64_t number{0};
    uint64_t timestamp{0};
    uint256 receipts_root{};
    Bytes header{};
    uint256 hash{};
    bool canonical{false};
    //! The parent's index in World::blocks, when the parent is in the world.
    std::optional<size_t> parent{};
    //! The receipts trie's inclusion proof of each transaction index.
    std::vector<std::vector<Bytes>> proofs{};
};

//! A header with the block's number, receipts root and time but a parent
//! no world block has, so it hashes to no link of the chain.
Bytes ForgedHeader(const ExecBlock& block)
{
    return ExecutionHeader(uint256{0x66}, block.number, block.receipts_root, block.timestamp);
}

struct World {
    //! The canonical chain by number, then the side-fork blocks.
    std::vector<ExecBlock> blocks{};
    size_t canonical{0};
    std::vector<Deposit> deposits{};
    //! Genuine deposits by id: a genuine id is unique.
    std::map<uint64_t, size_t> genuine{};
    std::map<uint256, size_t> by_hash{};

    //! Headers from block `from` down its parents to the number of block
    //! `to`, ending with `to`'s own header: the ancestry a record carries.
    std::vector<Bytes> Ancestry(size_t from, const size_t to) const
    {
        std::vector<Bytes> headers;
        while (blocks[from].number > blocks[to].number && blocks[from].parent) {
            headers.push_back(blocks[from].header);
            from = *blocks[from].parent;
        }
        headers.push_back(blocks[to].header);
        return headers;
    }
};

World BuildWorld(FuzzedDataProvider& fdp)
{
    World world;
    const bridge::EthAddress& vault{Bridge().asset.vault_address};
    const bridge::EthAddress& token{Bridge().asset.token_address};
    Word deposit_topic;
    std::copy(bridge::DepositTopic().begin(), bridge::DepositTopic().end(), deposit_topic.begin());
    static const std::array<unsigned char, 256> bloom{};
    uint64_t next_id{fdp.PickValueInArray<uint64_t>({0, 7'000, std::numeric_limits<uint64_t>::max() - 512})};

    const auto add_block{[&](ExecBlock block, const uint256& parent_hash) {
        const size_t index{world.blocks.size()};
        bridge_test::EthTrie trie;
        const uint64_t receipts{fdp.ConsumeIntegralInRange<uint64_t>(1, 3)};
        for (uint64_t tx_index{0}; tx_index < receipts; ++tx_index) {
            // Mostly a successful legacy or typed receipt; at times a
            // reverted one, or one of a type the V1 rules cannot read. (The
            // lowest choice, which an exhausted input makes, is the honest
            // one here and below.)
            const bool success{fdp.ConsumeIntegralInRange<int>(0, 7) != 7};
            const uint8_t type{fdp.PickValueInArray<uint8_t>({0, 0, 2, 4, 5})};
            std::vector<Bytes> logs;
            const uint32_t log_count{(fdp.ConsumeIntegralInRange<uint32_t>(0, 3) + 1) % 4};
            for (uint32_t log_index{0}; log_index < log_count; ++log_index) {
                const int kind{fdp.ConsumeIntegralInRange<int>(0, 9)};
                if (kind == 9) {
                    // Another event of the vault, not a Deposit.
                    logs.push_back(EncodeLog(vault, {UintWord(fdp.ConsumeIntegral<uint64_t>())}, Bytes(32, 0x01)));
                    continue;
                }
                Deposit deposit;
                deposit.block = index;
                deposit.tx_index = tx_index;
                deposit.log_index = log_index;
                deposit.amount = ConsumeAmount(fdp);
                deposit.recipient = ConsumeRecipient(fdp);
                deposit.genuine = kind < 6 && block.canonical && success;
                deposit.provable = deposit.genuine && type <= V1_MAX_RECEIPT_TYPE;
                // A lookalike takes a fresh id or reuses a genuine one.
                if (!deposit.genuine && !world.genuine.empty() && fdp.ConsumeBool()) {
                    deposit.id = std::next(world.genuine.begin(),
                                           fdp.ConsumeIntegralInRange<size_t>(0, world.genuine.size() - 1))
                                     ->first;
                } else {
                    deposit.id = next_id++;
                }
                bridge::EthAddress emitter{vault};
                std::vector<Word> topics{deposit_topic, UintWord(deposit.id), AddressWord(token)};
                Bytes data(deposit.amount.begin(), deposit.amount.end());
                data.insert(data.end(), deposit.recipient.begin(), deposit.recipient.end());
                switch (kind) {
                case 6: emitter = token; break;    // another contract's event
                case 7: topics[2][31] ^= 1; break; // a deposit of another token
                case 8:                            // a Deposit log of the wrong shape
                    switch (fdp.ConsumeIntegralInRange<int>(0, 6)) {
                    case 0: topics.pop_back(); break;
                    case 1: topics.push_back(UintWord(1)); break;
                    case 2: data.pop_back(); break;
                    case 3: data.push_back(0); break;
                    case 4: topics[1][fdp.ConsumeIntegralInRange<size_t>(0, 23)] = 1; break;
                    case 5: topics[2][fdp.ConsumeIntegralInRange<size_t>(0, 11)] = 1; break;
                    default: topics[0][0] ^= 1; break;
                    }
                    break;
                default: break;
                }
                logs.push_back(EncodeLog(emitter, topics, data));
                if (deposit.genuine) world.genuine.emplace(deposit.id, world.deposits.size());
                world.deposits.push_back(deposit);
            }
            const Bytes fields{bridge::RlpEncodeList({bridge::RlpEncodeUint64(success ? 1 : 0),
                                                      bridge::RlpEncodeUint64(21'000 * (tx_index + 1)),
                                                      bridge::RlpEncodeBytes(bloom), bridge::RlpEncodeList(logs)})};
            Bytes receipt;
            if (type != 0) receipt.push_back(type);
            receipt.insert(receipt.end(), fields.begin(), fields.end());
            trie.Insert(bridge::RlpEncodeUint64(tx_index), receipt);
        }
        block.receipts_root = trie.Root();
        for (uint64_t tx_index{0}; tx_index < receipts; ++tx_index) {
            block.proofs.push_back(trie.Proof(bridge::RlpEncodeUint64(tx_index)));
        }
        block.header = ExecutionHeader(parent_hash, block.number, block.receipts_root, block.timestamp);
        block.hash = Keccak(block.header);
        world.by_hash.emplace(block.hash, index);
        world.blocks.push_back(std::move(block));
    }};

    // The canonical chain starts up to three blocks before the vault's
    // deployment block.
    const uint64_t first{*Bridge().origin_deployment_block - fdp.ConsumeIntegralInRange<uint64_t>(0, 3)};
    const size_t length{fdp.ConsumeIntegralInRange<size_t>(4, 20)};
    uint256 parent{0x55};
    for (size_t i{0}; i < length; ++i) {
        ExecBlock block;
        block.number = first + i;
        block.timestamp = FIRST_TIMESTAMP + 12 * i;
        block.canonical = true;
        if (i > 0) block.parent = i - 1;
        add_block(std::move(block), parent);
        parent = world.blocks.back().hash;
    }
    world.canonical = length;
    // A side fork: blocks on canonical parents that no head finalizes.
    const size_t forks{fdp.ConsumeIntegralInRange<size_t>(0, 2)};
    for (size_t f{0}; f < forks; ++f) {
        const size_t on{fdp.ConsumeIntegralInRange<size_t>(0, length - 1)};
        ExecBlock block;
        block.number = world.blocks[on].number + 1;
        block.timestamp = world.blocks[on].timestamp + 13 + f;
        block.parent = on;
        add_block(std::move(block), world.blocks[on].hash);
    }
    return world;
}

//! The one type-10 record a transaction carries, decoded.
std::optional<bridge::BridgeRecordV1> BridgeRecordOf(const CTransaction& tx)
{
    for (const CMpaRecord& record : tx.mpa) {
        if (record.payload_type == bridge::BRIDGE_MPA_TYPE) {
            return bridge::DecodeBridgeMpaRecordV1(record, bridge::BRIDGE_MPT_LIMITS_V1);
        }
    }
    return std::nullopt;
}

bool SameDelta(const node::BridgeBlockDelta& a, const node::BridgeBlockDelta& b)
{
    return a.height == b.height && a.block_hash == b.block_hash && a.previous_height == b.previous_height &&
           a.previous_block_hash == b.previous_block_hash && a.light_client_before == b.light_client_before &&
           a.light_client_after == b.light_client_after &&
           a.light_client_connection_before == b.light_client_connection_before &&
           a.light_client_connection_after == b.light_client_connection_after &&
           a.anchors_added == b.anchors_added && a.mint_authorizations == b.mint_authorizations &&
           a.nullifiers_added == b.nullifiers_added && a.epoch_mint_changes == b.epoch_mint_changes &&
           a.withdrawals_added == b.withdrawals_added &&
           a.decentralized_withdrawals_added == b.decentralized_withdrawals_added &&
           a.withdrawal_tree_before == b.withdrawal_tree_before &&
           a.withdrawal_tree_after == b.withdrawal_tree_after && a.v2_store == b.v2_store &&
           a.withdrawals_v2_added == b.withdrawals_v2_added &&
           a.withdrawal_tree_v2_before == b.withdrawal_tree_v2_before &&
           a.withdrawal_tree_v2_after == b.withdrawal_tree_v2_after &&
           a.observations_v2_before == b.observations_v2_before &&
           a.observations_v2_added == b.observations_v2_added && a.reserve_v2_before == b.reserve_v2_before &&
           a.reserve_v2_after == b.reserve_v2_after && a.deposits_v2_added == b.deposits_v2_added &&
           a.conflict_v2_before == b.conflict_v2_before && a.conflict_v2_after == b.conflict_v2_after &&
           a.finality_conflicts_v2_added == b.finality_conflicts_v2_added &&
           a.reserve_v2_successor_before == b.reserve_v2_successor_before &&
           a.reserve_v2_successor_after == b.reserve_v2_successor_after &&
           a.deposits_v2_successor_added == b.deposits_v2_successor_added;
}

//! Everything the index answers, through its public queries, about the
//! world and the heights a run can reach.
struct Snapshot {
    int height{-1};
    uint256 hash{};
    std::optional<bridge::LightClientStore> light_client{};
    std::optional<node::BridgeStateConnection> light_client_connection{};
    std::vector<std::optional<std::pair<bridge::LightClientStore, node::BridgeStateConnection>>> finalized{};
    std::vector<size_t> counts{};
    std::vector<bool> nullified{};
    std::vector<std::optional<node::BridgeStateConnection>> nullifier_connections{};
    std::vector<std::optional<node::BridgeExecutionAnchor>> anchors{};
    std::vector<std::optional<node::BridgeExecutionAnchor>> anchors_for_target{};
    std::vector<CAmount> epoch_minted{};
    modern::WithdrawalTreeState tree{};
    std::vector<std::optional<uint256>> roots{};
    std::vector<std::optional<node::BridgeDecentralizedWithdrawalRequest>> withdrawals{};
    std::optional<std::vector<node::BridgeDecentralizedWithdrawalRequest>> withdrawals_through{};

    friend bool operator==(const Snapshot&, const Snapshot&) = default;
};

//! The model's state after a connected block.
struct Model {
    uint64_t light_client_slot{0};
    size_t light_client_block{0};
    node::BridgeStateConnection light_client_connection{};
    std::map<uint256, node::BridgeExecutionAnchor> anchors{};
    std::map<uint64_t, CAmount> epoch_minted{};
    //! Minted deposit ids and the block that minted each.
    std::map<uint64_t, node::BridgeStateConnection> minted{};
    std::map<node::BridgeWithdrawalId, uint64_t> burn_sources{};
    std::vector<node::BridgeDecentralizedWithdrawalRequest> withdrawals{};
    modern::WithdrawalTreeState tree{};
    std::map<int, uint256> roots{};

    CAmount EpochMinted(const uint64_t epoch) const
    {
        const auto found{epoch_minted.find(epoch)};
        return found == epoch_minted.end() ? 0 : found->second;
    }
};

//! What the model knows of a candidate transaction.
enum class Shape {
    PLAIN,    //!< no bridge material: no effect
    MINT,     //!< an honest MINT of `deposit` naming the block `anchor`
    BACKFILL, //!< an honest EXECUTION_BACKFILL from block `anchor` to `target`
    BURN,     //!< an honest BRIDGE_BURN of `amount` to `recipient`
    REFUSED,  //!< a defect every node refuses, whatever the state
    UNKNOWN,  //!< a defect or arbitrary record the model does not judge
};

struct Planned {
    CTransactionRef tx{};
    Shape shape{Shape::PLAIN};
    size_t deposit{0};
    uint256 anchor{};
    size_t target{0};
    uint64_t amount{0};
    bridge::EthAddress recipient{};
};

class Harness
{
public:
    Harness(const World& world, FuzzedDataProvider& fdp);

    void Candidate();
    void AdvanceLightClient();
    void Disconnect();
    void Reconnect();
    void CheckIndex() const;
    void CheckReplay() const;

private:
    struct Entry {
        int height{-1};
        uint256 hash{};
        //! The block VerifyBlock judged; none for a hand-built light-client
        //! delta, whose finalized block and slot follow.
        std::optional<CBlock> block{};
        std::pair<size_t, uint64_t> light_client{};
        node::BridgeBlockDelta delta{};
        Model model{};
        Snapshot snapshot{};
    };

    struct Verdict {
        bool known{false};
        bool accept{false};
        node::BridgeBlockDelta delta{};
    };

    const World& m_world;
    FuzzedDataProvider& m_fdp;
    node::BridgeStateIndex m_index;
    //! The active chain from the seed.
    std::vector<Entry> m_chain;
    //! The block just disconnected, while the tip is its parent.
    std::optional<Entry> m_detached;
    //! Transactions of recent candidates, to repeat.
    std::vector<Planned> m_built;
    uint64_t m_counter{0};
    int m_seed_height{-1};

    const Entry& Tip() const { return m_chain.back(); }
    uint256 NextHash();
    std::vector<uint64_t> Epochs() const;
    Snapshot TakeSnapshot(const node::BridgeStateIndex& index) const;
    node::BridgeBlockDelta LightClientDelta(const node::BridgeStateIndex& index, int height, const uint256& hash,
                                            size_t block, uint64_t slot) const;
    void ApplyDelta(Model& model, const node::BridgeBlockDelta& delta) const;
    void Connect(const node::BridgeBlockDelta& delta, std::optional<CBlock> block,
                 std::pair<size_t, uint64_t> light_client);
    uint32_t ConsumeTime();
    CTransactionRef MakeTx(std::vector<CTxOut> vout, std::vector<CMpaRecord> mpa);
    std::vector<size_t> AnchorChoices(const std::vector<Planned>& earlier) const;
    Planned MakeTransaction(const std::vector<Planned>& earlier);
    Planned MakeMint(const std::vector<Planned>& earlier);
    Planned MakeBackfill(const std::vector<Planned>& earlier);
    Planned MakeBurn();
    Planned MakePlain();
    Planned MakeRefused();
    Planned MakeArbitrary();
    Verdict Predict(const std::vector<Planned>& planned, int height, uint32_t time, const uint256& hash) const;
    void CheckSafety(const CBlock& block, const node::BridgeBlockDelta& delta) const;
    void CheckTransactions(const std::vector<Planned>& planned, int height, uint32_t time,
                           const uint256& hash) const;
    void CheckPreview(const std::vector<Planned>& planned, int height, uint32_t time, const uint256& hash,
                      bool accepted);
};

Harness::Harness(const World& world, FuzzedDataProvider& fdp) : m_world{world}, m_fdp{fdp}
{
    // The light client bootstraps in the activation block, or a few blocks
    // before a later epoch boundary so the run crosses it.
    m_seed_height = Activation();
    if (const int epochs{m_fdp.ConsumeIntegralInRange<int>(0, 2)}; epochs > 0) {
        m_seed_height += epochs * static_cast<int>(Caps().epoch_length_blocks) -
                         m_fdp.ConsumeIntegralInRange<int>(1, 4);
    }
    // The head is in the upper half of the canonical chain. Its slot is
    // mostly well inside the pinned fork schedule, at times an advance or
    // two from its end, and at times past it.
    const size_t finalized{m_fdp.ConsumeIntegralInRange<size_t>(m_world.canonical / 2, m_world.canonical - 1)};
    const uint64_t last_slot{(Pins().fork_schedule_valid_through_epoch + 1) * bridge::SLOTS_PER_EPOCH - 1};
    uint64_t slot{last_slot - m_fdp.PickValueInArray<uint64_t>({4'096, 160, 96, 1, 0})};
    if (m_fdp.ConsumeIntegralInRange<int>(0, 7) == 7) slot = last_slot + m_fdp.ConsumeIntegralInRange<uint64_t>(1, 64);
    const uint256 seed_hash{NextHash()};
    Connect(LightClientDelta(m_index, m_seed_height, seed_hash, finalized, slot), std::nullopt, {finalized, slot});

    // The rules are off below activation and need a block hash.
    const CBlock empty;
    node::BridgeBlockDelta delta;
    std::string error;
    assert(!m_index.VerifyBlock(empty, Activation() - 1, NextHash(), *g_params, delta, error));
    assert(!m_index.BeginBlockPreview(Activation() - 1, empty.nTime, NextHash(), *g_params, error));
    assert(!m_index.VerifyBlock(empty, Tip().height + 1, uint256{}, *g_params, delta, error));
}

uint256 Harness::NextHash()
{
    uint256 hash;
    WriteBE64(hash.begin(), ++m_counter);
    *(hash.end() - 1) = 0xb3;
    return hash;
}

std::vector<uint64_t> Harness::Epochs() const
{
    const uint64_t first{Epoch(m_seed_height + 1)};
    return {first, first + 1, first + 2};
}

Snapshot Harness::TakeSnapshot(const node::BridgeStateIndex& index) const
{
    Snapshot out;
    out.height = index.ConnectedHeight();
    out.hash = index.ConnectedHash();
    out.light_client = index.LightClient();
    out.light_client_connection = index.LightClientLastConnected();
    for (const int height : {m_seed_height - 1, m_seed_height, out.height - 1, out.height}) {
        const auto finalized{index.FinalizedLightClientSnapshot(height)};
        out.finalized.emplace_back();
        if (finalized) out.finalized.back().emplace(finalized->store, finalized->connection);
    }
    out.counts = {index.NullifierCount(), index.NullifierCount(bridge::BRIDGE_NULLIFIER_NAMESPACE_V1),
                  index.AnchorCount(), index.WithdrawalCount(), index.DecentralizedWithdrawalCount(),
                  index.History().size()};
    for (const Deposit& deposit : m_world.deposits) {
        out.nullified.push_back(index.IsNullified(Key(deposit.id)));
        out.nullifier_connections.push_back(index.NullifierConnection(Key(deposit.id)));
    }
    for (const ExecBlock& block : m_world.blocks) {
        out.anchors.push_back(index.Anchor(block.hash));
        out.anchors_for_target.push_back(index.AnchorForTarget(block.number, out.height));
    }
    for (const uint64_t epoch : Epochs()) out.epoch_minted.push_back(index.EpochMinted(g_asset, epoch));
    out.tree = index.WithdrawalTree();
    for (int height{m_seed_height - 1}; height <= m_seed_height + static_cast<int>(MAX_STEPS) + 1; ++height) {
        out.roots.push_back(index.WithdrawalRootAtHeight(height));
    }
    for (uint64_t id{0}; id <= index.DecentralizedWithdrawalCount(); ++id) {
        out.withdrawals.push_back(index.DecentralizedWithdrawal(id));
    }
    out.withdrawals_through = index.DecentralizedWithdrawalsThrough(out.height);
    return out;
}

//! The delta of a block whose one accepted BOOTSTRAP or UPDATE record makes
//! `block` the finalized execution head at `slot`, against `index`.
node::BridgeBlockDelta Harness::LightClientDelta(const node::BridgeStateIndex& index, const int height,
                                                 const uint256& hash, const size_t block,
                                                 const uint64_t slot) const
{
    const ExecBlock& exec{m_world.blocks[block]};
    bridge::LightClientStore store;
    store.finalized_header.beacon.slot = slot;
    store.finalized_header.execution.block_number = exec.number;
    store.finalized_header.execution.block_hash = exec.hash;
    store.finalized_header.execution.receipts_root = exec.receipts_root;
    store.finalized_header.execution.timestamp = exec.timestamp;
    store.period = bridge::PeriodAtSlot(slot);

    node::BridgeBlockDelta delta;
    delta.height = height;
    delta.block_hash = hash;
    delta.previous_height = index.ConnectedHeight();
    delta.previous_block_hash = index.ConnectedHash();
    delta.light_client_before = index.LightClient();
    delta.light_client_after = store;
    delta.light_client_connection_before = index.LightClientLastConnected();
    delta.light_client_connection_after = node::BridgeStateConnection{height, hash};
    // A head that keeps its execution block (a later slot only) adds no
    // anchor: the block is retained already.
    if (!index.Anchor(exec.hash)) {
        delta.anchors_added.push_back(node::BridgeExecutionAnchor{exec.number, exec.hash, exec.receipts_root, slot,
                                                                  exec.number, exec.timestamp, height, hash});
    }
    delta.withdrawal_tree_before = index.WithdrawalTree();
    delta.withdrawal_tree_after = index.WithdrawalTree();
    return delta;
}

void Harness::ApplyDelta(Model& model, const node::BridgeBlockDelta& delta) const
{
    if (delta.light_client_after) {
        model.light_client_slot = delta.light_client_after->finalized_header.beacon.slot;
        model.light_client_block = m_world.by_hash.at(delta.light_client_after->finalized_header.execution.block_hash);
        model.light_client_connection = *delta.light_client_connection_after;
    }
    for (const node::BridgeExecutionAnchor& anchor : delta.anchors_added) model.anchors.emplace(anchor.block_hash, anchor);
    for (const node::BridgeTxMintAuthorization& mint : delta.mint_authorizations) {
        model.minted.emplace(mint.authorization.nullifier.deposit_id,
                             node::BridgeStateConnection{delta.height, delta.block_hash});
        model.epoch_minted[Epoch(delta.height)] += mint.authorization.amount;
    }
    for (const node::BridgeDecentralizedWithdrawalRequest& request : delta.decentralized_withdrawals_added) {
        model.burn_sources.emplace(node::BridgeWithdrawalId{request.transaction_id, request.burn_output_index},
                                   request.withdrawal.withdrawal_id);
        model.withdrawals.push_back(request);
        assert(modern::AppendWithdrawalLeaf(model.tree, request.leaf));
    }
    model.roots[delta.height] = model.tree.root;
}

void Harness::Connect(const node::BridgeBlockDelta& delta, std::optional<CBlock> block,
                      const std::pair<size_t, uint64_t> light_client)
{
    std::string error;
    assert(m_index.ConnectBlock(delta, error));
    Entry entry;
    entry.height = delta.height;
    entry.hash = delta.block_hash;
    entry.block = std::move(block);
    entry.light_client = light_client;
    entry.delta = delta;
    if (!m_chain.empty()) entry.model = Tip().model;
    ApplyDelta(entry.model, delta);
    entry.snapshot = TakeSnapshot(m_index);
    m_chain.push_back(std::move(entry));
    m_detached.reset();
}

uint32_t Harness::ConsumeTime()
{
    // Mostly around the head's age limit.
    const int64_t finalized{static_cast<int64_t>(m_world.blocks[Tip().model.light_client_block].timestamp)};
    const int64_t lag{static_cast<int64_t>(Pins().max_sync_lag_slots) * 12};
    int64_t offset{0};
    switch (m_fdp.ConsumeIntegralInRange<int>(0, 4)) {
    case 0: offset = lag + m_fdp.ConsumeIntegralInRange<int64_t>(-1, 1); break;
    case 1: offset = m_fdp.ConsumeIntegralInRange<int64_t>(-600, 600); break;
    case 2: offset = lag - m_fdp.ConsumeIntegralInRange<int64_t>(0, 3'600); break;
    case 3: offset = m_fdp.ConsumeIntegralInRange<int64_t>(-lag, 3 * lag); break;
    default: offset = 12 * m_fdp.ConsumeIntegralInRange<int64_t>(0, 120); break;
    }
    return static_cast<uint32_t>(finalized + offset);
}

CTransactionRef Harness::MakeTx(std::vector<CTxOut> vout, std::vector<CMpaRecord> mpa)
{
    CMutableTransaction tx;
    // Mostly spending an outpoint of its own, so equal records still make
    // distinct transactions; at times no input at all.
    if (m_fdp.ConsumeIntegralInRange<int>(0, 3) != 3) tx.vin.emplace_back(COutPoint{Txid::FromUint256(NextHash()), 0});
    tx.vout = std::move(vout);
    tx.mpa = std::move(mpa);
    return MakeTransactionRef(std::move(tx));
}

//! Blocks a record may name as its anchor: the retained ones and those an
//! earlier honest backfill of the candidate would add.
std::vector<size_t> Harness::AnchorChoices(const std::vector<Planned>& earlier) const
{
    std::vector<size_t> out;
    out.reserve(Tip().model.anchors.size() + earlier.size());
    for (const auto& [hash, anchor] : Tip().model.anchors) out.push_back(m_world.by_hash.at(hash));
    for (const Planned& planned : earlier) {
        if (planned.shape == Shape::BACKFILL) out.push_back(planned.target);
    }
    return out;
}

Planned Harness::MakeTransaction(const std::vector<Planned>& earlier)
{
    switch (m_fdp.ConsumeIntegralInRange<int>(0, 11)) {
    case 0:
    case 1:
    case 2:
    case 3: return MakeMint(earlier);
    case 4:
    case 5: return MakeBackfill(earlier);
    case 6:
    case 7: return MakeBurn();
    case 8: return MakePlain();
    case 9: return MakeRefused();
    case 10: {
        // A transaction built before, in this candidate or a recent one.
        std::vector<Planned> before{m_built};
        before.insert(before.end(), earlier.begin(), earlier.end());
        if (before.empty()) return MakePlain();
        return Pick(m_fdp, before);
    }
    default: return MakeArbitrary();
    }
}

Planned Harness::MakeMint(const std::vector<Planned>& earlier)
{
    if (m_world.deposits.empty()) return MakePlain();
    Planned out;
    out.shape = Shape::MINT;
    const std::vector<size_t> choices{AnchorChoices(earlier)};
    // Mostly a deposit the chain could mint now: genuine, readable, from
    // the vault's deployment on, at or below an anchor, and neither minted
    // nor planned yet. Else any genuine deposit, or any Deposit-shaped log.
    std::vector<size_t> mintable;
    for (const auto& [id, index] : m_world.genuine) {
        const Deposit& deposit{m_world.deposits[index]};
        const uint64_t number{m_world.blocks[deposit.block].number};
        const auto planned{[&](const Planned& p) {
            return p.shape == Shape::MINT && m_world.deposits[p.deposit].id == id;
        }};
        const auto covers{[&](const size_t block) { return m_world.blocks[block].number >= number; }};
        if (deposit.provable && number >= *Bridge().origin_deployment_block && ReferenceAmount(deposit.amount) &&
            ReferenceRecipient(deposit.recipient) && !Tip().model.minted.contains(id) &&
            std::none_of(earlier.begin(), earlier.end(), planned) &&
            std::any_of(choices.begin(), choices.end(), covers)) {
            mintable.push_back(index);
        }
    }
    switch (m_fdp.ConsumeIntegralInRange<int>(0, 3)) {
    case 0:
    case 1:
        if (!mintable.empty()) {
            out.deposit = Pick(m_fdp, mintable);
            break;
        }
        [[fallthrough]];
    case 2:
        if (!m_world.genuine.empty()) {
            out.deposit = std::next(m_world.genuine.begin(),
                                    m_fdp.ConsumeIntegralInRange<size_t>(0, m_world.genuine.size() - 1))
                              ->second;
            break;
        }
        [[fallthrough]];
    default: out.deposit = m_fdp.ConsumeIntegralInRange<size_t>(0, m_world.deposits.size() - 1); break;
    }
    const Deposit& deposit{m_world.deposits[out.deposit]};
    const ExecBlock& target{m_world.blocks[deposit.block]};

    // Mostly an anchor at or above the deposit's block, else any anchor or
    // any block.
    std::vector<size_t> above;
    std::copy_if(choices.begin(), choices.end(), std::back_inserter(above),
                 [&](const size_t block) { return m_world.blocks[block].number >= target.number; });
    size_t anchor{0};
    if (!above.empty() && m_fdp.ConsumeIntegralInRange<int>(0, 7) != 7) {
        anchor = Pick(m_fdp, above);
    } else if (!choices.empty() && m_fdp.ConsumeBool()) {
        anchor = Pick(m_fdp, choices);
    } else {
        anchor = m_fdp.ConsumeIntegralInRange<size_t>(0, m_world.blocks.size() - 1);
    }
    out.anchor = m_world.blocks[anchor].hash;

    std::vector<Bytes> headers{m_world.Ancestry(anchor, deposit.block)};
    if (headers.size() > bridge::MAX_BRIDGE_EXECUTION_ANCESTRY_HEADERS) {
        // Too far above for one record: the walk stops short of the deposit.
        headers.resize(bridge::MAX_BRIDGE_EXECUTION_ANCESTRY_HEADERS);
        out.shape = Shape::REFUSED;
    }
    bridge::BridgeMintV1 mint{g_registry, 0, out.anchor, target.number, deposit.tx_index, deposit.log_index,
                              std::move(headers), target.proofs[deposit.tx_index]};
    // The output an honest mint pays, or the lenient one.
    CAmount amount{ReferenceAmount(deposit.amount).value_or(LenientAmount(deposit.amount))};
    CScript recipient{ReferenceRecipient(deposit.recipient).value_or(LenientRecipient(deposit.recipient))};
    switch (m_fdp.ConsumeIntegralInRange<int>(0, 24)) {
    case 16: // pays one unit more, or less, than the deposit
        amount += amount < MAX_MONEY ? 1 : -1;
        out.shape = Shape::REFUSED;
        break;
    case 17: // pays someone else
        recipient = P2PKH(0x78);
        out.shape = Shape::REFUSED;
        break;
    case 18: // names the binding output
        mint.output_index = 1;
        out.shape = Shape::REFUSED;
        break;
    case 19: // names another registry
        mint.registry_id = uint256{0x42};
        out.shape = Shape::REFUSED;
        break;
    case 20: // carries a header after the deposit's
        mint.ancestry_headers.push_back(target.header);
        out.shape = Shape::REFUSED;
        break;
    case 21: // claims the next block number
        mint.target_block_number += 1;
        out.shape = Shape::REFUSED;
        break;
    case 22: // names another log or receipt of the block
        if (m_fdp.ConsumeBool()) {
            mint.receipt_log_index += 1;
        } else {
            mint.tx_index += 1;
        }
        out.shape = Shape::UNKNOWN;
        break;
    case 23: { // grafts another deposit's receipt proof onto this ancestry
        const Deposit& other{Pick(m_fdp, m_world.deposits)};
        mint.tx_index = other.tx_index;
        mint.receipt_log_index = other.log_index;
        mint.mpt_nodes = m_world.blocks[other.block].proofs[other.tx_index];
        amount = ReferenceAmount(other.amount).value_or(LenientAmount(other.amount));
        recipient = ReferenceRecipient(other.recipient).value_or(LenientRecipient(other.recipient));
        // Another block's proof is under another root, unless both
        // blocks hold the same receipts.
        out.shape = m_world.blocks[other.block].receipts_root == target.receipts_root ? Shape::UNKNOWN
                                                                                      : Shape::REFUSED;
        break;
    }
    case 24: // carries a forged copy of the deposit's header
        mint.ancestry_headers.back() = ForgedHeader(target);
        out.shape = Shape::REFUSED;
        break;
    default: break;
    }
    const auto record{bridge::MakeBridgeMpaRecord(bridge::BridgeRecordV1{bridge::BridgeRecordKindV1::MINT, std::move(mint)})};
    const auto owner{modern::MakeAssetOwnerOutput(g_asset, amount, recipient)};
    if (!record || !owner) return MakePlain();
    out.tx = MakeTx({*owner, *modern::MakeBridgeBindingOutput(*record)}, {*record});
    return out;
}

Planned Harness::MakeBackfill(const std::vector<Planned>& earlier)
{
    Planned out;
    out.shape = Shape::BACKFILL;
    const std::vector<size_t> choices{AnchorChoices(earlier)};
    size_t source{0};
    if (!choices.empty() && m_fdp.ConsumeIntegralInRange<int>(0, 7) != 7) {
        source = Pick(m_fdp, choices);
    } else {
        source = m_fdp.ConsumeIntegralInRange<size_t>(0, m_world.blocks.size() - 1);
    }
    out.anchor = m_world.blocks[source].hash;
    // One of the source block's ancestors, at most 31 down, or the source
    // block itself.
    out.target = source;
    for (int steps{(m_fdp.ConsumeIntegralInRange<int>(0, 31) + 1) % 32};
         steps > 0 && m_world.blocks[out.target].parent; --steps) {
        out.target = *m_world.blocks[out.target].parent;
    }
    bridge::BridgeExecutionBackfillV1 backfill{out.anchor, m_world.blocks[out.target].number,
                                               m_world.Ancestry(source, out.target)};
    switch (m_fdp.ConsumeIntegralInRange<int>(0, 10)) {
    case 8: // carries a header after the target's
        backfill.ancestry_headers.push_back(m_world.blocks[out.target].header);
        out.shape = Shape::REFUSED;
        break;
    case 9: // claims the next block number
        backfill.target_block_number += 1;
        out.shape = Shape::REFUSED;
        break;
    case 10: // carries a forged copy of the target's header
        backfill.ancestry_headers.back() = ForgedHeader(m_world.blocks[out.target]);
        out.shape = Shape::REFUSED;
        break;
    default: break;
    }
    const auto record{bridge::MakeBridgeMpaRecord(
        bridge::BridgeRecordV1{bridge::BridgeRecordKindV1::EXECUTION_BACKFILL, std::move(backfill)})};
    if (!record) return MakePlain();
    out.tx = MakeTx({*modern::MakeBridgeBindingOutput(*record)}, {*record});
    return out;
}

Planned Harness::MakeBurn()
{
    Planned out;
    out.shape = Shape::BURN;
    const uint64_t money{static_cast<uint64_t>(MAX_MONEY)};
    out.amount = m_fdp.PickValueInArray<uint64_t>(
        {1, 1'000'000, static_cast<uint64_t>(Caps().max_per_block), money, money + 1});
    out.recipient.fill(m_fdp.ConsumeIntegralInRange<unsigned char>(1, 3));
    bridge::BridgeBurnV1 burn{g_registry, 0, out.amount, out.recipient, std::nullopt};
    CAmount burned{static_cast<CAmount>(std::min(out.amount, money))};
    bool paid{false};
    switch (m_fdp.ConsumeIntegralInRange<int>(0, 11)) {
    case 8: // burns one unit more, or less, than the record names
        burned += burned < MAX_MONEY ? 1 : -1;
        out.shape = Shape::REFUSED;
        break;
    case 9: // names another registry
        burn.registry_id = uint256{0x42};
        out.shape = Shape::REFUSED;
        break;
    case 10: // names the binding output
        burn.burn_output_index = 1;
        out.shape = Shape::REFUSED;
        break;
    case 11: // pays the bUSD to an owner instead of burning it
        paid = true;
        out.shape = Shape::REFUSED;
        break;
    default: break;
    }
    const auto record{
        bridge::MakeBridgeMpaRecord(bridge::BridgeRecordV1{bridge::BridgeRecordKindV1::BRIDGE_BURN, burn})};
    const auto output{paid ? modern::MakeAssetOwnerOutput(g_asset, burned, P2PKH(0x77))
                           : modern::MakeAssetBurnOutput(g_asset, burned)};
    assert(record && output);
    out.tx = MakeTx({*output, *modern::MakeBridgeBindingOutput(*record)}, {*record});
    return out;
}

Planned Harness::MakePlain()
{
    Planned out;
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(NextHash()), 0});
    // At times a coinbase, which needs no more without bridge material.
    if (m_fdp.ConsumeIntegralInRange<int>(0, 3) == 3) tx.vin[0].prevout.SetNull();
    tx.vout.emplace_back(1'000, P2PKH(0x77));
    // At times a record of another payload type, which the bridge ignores.
    if (m_fdp.ConsumeBool()) {
        const auto type{static_cast<uint16_t>(bridge::BRIDGE_MPA_TYPE + m_fdp.ConsumeIntegralInRange<int>(1, 3))};
        tx.mpa.push_back(CMpaRecord{type, 1, Bytes(8, 0x5a)});
    }
    out.tx = MakeTransactionRef(std::move(tx));
    return out;
}

Planned Harness::MakeRefused()
{
    Planned out;
    out.shape = Shape::REFUSED;
    // A small record to misuse: a backfill to any block.
    const ExecBlock& block{Pick(m_fdp, m_world.blocks)};
    const auto backfill{[&](const uint64_t number) {
        return bridge::MakeBridgeMpaRecord(bridge::BridgeRecordV1{
            bridge::BridgeRecordKindV1::EXECUTION_BACKFILL,
            bridge::BridgeExecutionBackfillV1{block.hash, number, {block.header}}});
    }};
    const auto record{backfill(block.number)};
    assert(record);
    const CTxOut binding{*modern::MakeBridgeBindingOutput(*record)};
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(NextHash()), 0});
    switch (m_fdp.ConsumeIntegralInRange<int>(0, 5)) {
    case 0: // a coinbase carrying a record
        tx.vin[0].prevout.SetNull();
        tx.vout = {binding};
        tx.mpa = {*record};
        break;
    case 1: // a binding output without its record
        tx.vout = {binding};
        break;
    case 2: // a record without its binding output
        tx.vout = {CTxOut{1'000, P2PKH(0x77)}};
        tx.mpa = {*record};
        break;
    case 3: // two binding outputs
        tx.vout = {binding, binding};
        tx.mpa = {*record};
        break;
    case 4: { // two records
        const auto second{backfill(block.number + 1)};
        assert(second);
        tx.vout = {binding, *modern::MakeBridgeBindingOutput(*second)};
        tx.mpa = {*record, *second};
        break;
    }
    default: { // a managed withdrawal, closed on mainnet
        const auto managed{bridge::MakeBridgeMpaRecord(
            bridge::BridgeRecordV1{bridge::BridgeRecordKindV1::MANAGED_WITHDRAWAL,
                                   bridge::BridgeManagedWithdrawalV1{g_registry, 0, 1'000, bridge::EthAddress{0x11}}})};
        assert(managed);
        tx.vout = {*modern::MakeAssetBurnOutput(g_asset, 1'000), *modern::MakeBridgeBindingOutput(*managed)};
        tx.mpa = {*managed};
        break;
    }
    }
    out.tx = MakeTransactionRef(std::move(tx));
    return out;
}

Planned Harness::MakeArbitrary()
{
    Planned out;
    out.shape = Shape::UNKNOWN;
    CMpaRecord record{bridge::BRIDGE_MPA_TYPE, bridge::BRIDGE_MPA_VERSION_V1,
                      m_fdp.ConsumeBytes<unsigned char>(m_fdp.ConsumeIntegralInRange<size_t>(1, 512))};
    std::vector<CTxOut> vout;
    if (const auto binding{modern::MakeBridgeBindingOutput(record)}) vout.push_back(*binding);
    out.tx = MakeTx(std::move(vout), {std::move(record)});
    return out;
}

//! The model's verdict on a candidate block: a defect every node refuses
//! settles it; a transaction the model does not judge leaves it open;
//! otherwise the honest records are judged in order against the model, and
//! an accepted block's exact delta follows.
Harness::Verdict Harness::Predict(const std::vector<Planned>& planned, const int height, const uint32_t time,
                                  const uint256& hash) const
{
    Verdict verdict;
    const auto shaped{[&](const Shape shape) {
        return std::any_of(planned.begin(), planned.end(), [&](const Planned& p) { return p.shape == shape; });
    }};
    if (shaped(Shape::REFUSED)) {
        verdict.known = true;
        return verdict;
    }
    if (shaped(Shape::UNKNOWN)) return verdict;
    verdict.known = true;

    const Model& base{Tip().model};
    Model next{base};
    node::BridgeBlockDelta& delta{verdict.delta};
    delta.height = height;
    delta.block_hash = hash;
    delta.previous_height = Tip().height;
    delta.previous_block_hash = Tip().hash;
    delta.withdrawal_tree_before = base.tree;
    const uint64_t epoch{Epoch(height)};
    const CAmount epoch_before{base.EpochMinted(epoch)};
    CAmount block_sum{0};
    const bool head_usable{bridge::EpochAtSlot(base.light_client_slot) <= Pins().fork_schedule_valid_through_epoch &&
                           Fresh(time, m_world.blocks[base.light_client_block].timestamp)};
    for (const Planned& p : planned) {
        switch (p.shape) {
        case Shape::PLAIN: break;
        case Shape::MINT: {
            const Deposit& deposit{m_world.deposits[p.deposit]};
            const ExecBlock& target{m_world.blocks[deposit.block]};
            const auto anchor{next.anchors.find(p.anchor)};
            const auto amount{ReferenceAmount(deposit.amount)};
            const auto recipient{ReferenceRecipient(deposit.recipient)};
            if (!head_usable || anchor == next.anchors.end() || anchor->second.block_number < target.number ||
                !deposit.provable || target.number < *Bridge().origin_deployment_block || !amount || !recipient ||
                next.minted.contains(deposit.id) || *amount > Caps().max_per_block - block_sum ||
                *amount > Caps().max_per_epoch - epoch_before - block_sum) {
                return verdict;
            }
            block_sum += *amount;
            next.minted.emplace(deposit.id, node::BridgeStateConnection{height, hash});
            delta.mint_authorizations.push_back(node::BridgeTxMintAuthorization{
                p.tx->GetPtxid(), 0, bridge::BridgeMintAuthorization{g_asset, *amount, *recipient, Key(deposit.id)}});
            delta.nullifiers_added.push_back(Key(deposit.id));
            break;
        }
        case Shape::BACKFILL: {
            const auto source{next.anchors.find(p.anchor)};
            const ExecBlock& target{m_world.blocks[p.target]};
            // A retained target makes the record a no-op, which is refused.
            if (source == next.anchors.end() || next.anchors.contains(target.hash)) return verdict;
            const node::BridgeExecutionAnchor added{target.number,
                                                    target.hash,
                                                    target.receipts_root,
                                                    source->second.source_finalized_beacon_slot,
                                                    source->second.source_finalized_execution_block,
                                                    target.timestamp,
                                                    height,
                                                    hash};
            next.anchors.emplace(target.hash, added);
            delta.anchors_added.push_back(added);
            break;
        }
        case Shape::BURN: {
            const node::BridgeWithdrawalId source{p.tx->GetHash(), 0};
            if (p.amount > static_cast<uint64_t>(MAX_MONEY) || next.burn_sources.contains(source)) return verdict;
            const modern::BridgeWithdrawalV1 withdrawal{next.tree.count,          Bridge().asset.origin_chain_id,
                                                        g_asset,                  Bridge().asset.token_address,
                                                        p.recipient,              static_cast<CAmount>(p.amount),
                                                        static_cast<uint64_t>(height)};
            const auto leaf{modern::BridgeWithdrawalLeafV1(withdrawal)};
            assert(leaf && modern::AppendWithdrawalLeaf(next.tree, *leaf));
            next.burn_sources.emplace(source, withdrawal.withdrawal_id);
            delta.decentralized_withdrawals_added.push_back(
                node::BridgeDecentralizedWithdrawalRequest{source.transaction_id, 0, withdrawal, *leaf, height, hash});
            break;
        }
        case Shape::REFUSED:
        case Shape::UNKNOWN: assert(false);
        }
    }
    if (block_sum > 0) {
        delta.epoch_mint_changes.push_back(
            node::BridgeEpochMintChange{g_asset, epoch, epoch_before, epoch_before + block_sum});
    }
    delta.withdrawal_tree_after = next.tree;
    verdict.accept = true;
    return verdict;
}

//! What an accepted block may do at all, judged against the world rather
//! than the model's verdict, so it holds for arbitrary records too.
void Harness::CheckSafety(const CBlock& block, const node::BridgeBlockDelta& delta) const
{
    const Model& base{Tip().model};
    const int height{delta.height};
    assert(delta.previous_height == Tip().height && delta.previous_block_hash == Tip().hash);
    // No light-client proof is valid here, the managed leg is closed and V2
    // never starts on mainnet.
    assert(!delta.light_client_before && !delta.light_client_after && !delta.light_client_connection_before &&
           !delta.light_client_connection_after);
    assert(!delta.v2_store && delta.withdrawals_added.empty() && delta.withdrawals_v2_added.empty() &&
           delta.observations_v2_added.empty() && delta.deposits_v2_added.empty() &&
           delta.finality_conflicts_v2_added.empty() && delta.deposits_v2_successor_added.empty());

    // Every record of an accepted block took effect, one effect each.
    size_t mints{0};
    size_t backfills{0};
    size_t burns{0};
    for (const CTransactionRef& tx : block.vtx) {
        const auto record{BridgeRecordOf(*tx)};
        if (!record) continue;
        switch (record->kind) {
        case bridge::BridgeRecordKindV1::MINT: ++mints; break;
        case bridge::BridgeRecordKindV1::EXECUTION_BACKFILL: ++backfills; break;
        case bridge::BridgeRecordKindV1::BRIDGE_BURN: ++burns; break;
        default: assert(false);
        }
    }
    assert(mints == delta.mint_authorizations.size() && backfills == delta.anchors_added.size() &&
           burns == delta.decentralized_withdrawals_added.size());

    // New anchors are canonical blocks below a retained anchor of the same
    // finalized origin.
    std::map<uint256, node::BridgeExecutionAnchor> anchors{base.anchors};
    for (const node::BridgeExecutionAnchor& anchor : delta.anchors_added) {
        const auto found{m_world.by_hash.find(anchor.block_hash)};
        assert(found != m_world.by_hash.end());
        const ExecBlock& exec{m_world.blocks[found->second]};
        assert(exec.canonical && anchor.block_number == exec.number && anchor.receipts_root == exec.receipts_root &&
               anchor.execution_timestamp == exec.timestamp && anchor.connected_height == height &&
               anchor.connected_block == delta.block_hash);
        assert(std::any_of(anchors.begin(), anchors.end(), [&](const auto& retained) {
            const node::BridgeExecutionAnchor& source{retained.second};
            return source.block_number >= anchor.block_number &&
                   source.source_finalized_beacon_slot == anchor.source_finalized_beacon_slot &&
                   source.source_finalized_execution_block == anchor.source_finalized_execution_block;
        }));
        assert(anchors.emplace(anchor.block_hash, anchor).second);
    }

    // Each mint pays one genuine canonical deposit, once, exactly, within
    // the caps, under a usable head.
    const uint64_t epoch{Epoch(height)};
    const CAmount epoch_before{base.EpochMinted(epoch)};
    const ExecBlock& head{m_world.blocks[base.light_client_block]};
    CAmount block_sum{0};
    std::set<uint64_t> minted;
    std::vector<bridge::BridgeDepositKey> keys;
    for (const node::BridgeTxMintAuthorization& mint : delta.mint_authorizations) {
        const bridge::BridgeMintAuthorization& authorization{mint.authorization};
        const auto tx{std::find_if(block.vtx.begin(), block.vtx.end(), [&](const CTransactionRef& candidate) {
            return candidate->GetPtxid() == mint.transaction_id;
        })};
        assert(tx != block.vtx.end());
        const auto record{BridgeRecordOf(**tx)};
        assert(record && record->kind == bridge::BridgeRecordKindV1::MINT);
        const auto& proof{std::get<bridge::BridgeMintV1>(record->payload)};
        assert(proof.registry_id == g_registry && proof.output_index == mint.output_index);
        assert(authorization.asset == g_asset && authorization.nullifier == Key(authorization.nullifier.deposit_id));
        const auto genuine{m_world.genuine.find(authorization.nullifier.deposit_id)};
        assert(genuine != m_world.genuine.end());
        const Deposit& deposit{m_world.deposits[genuine->second]};
        assert(m_world.blocks[deposit.block].number >= *Bridge().origin_deployment_block);
        // ... in a block the light client finalized: at or below an anchor.
        assert(std::any_of(anchors.begin(), anchors.end(), [&](const auto& retained) {
            return retained.second.block_number >= m_world.blocks[deposit.block].number;
        }));
        const auto amount{ReferenceAmount(deposit.amount)};
        const auto recipient{ReferenceRecipient(deposit.recipient)};
        assert(amount && recipient && authorization.amount == *amount && authorization.recipient_script == *recipient);
        assert(!base.minted.contains(deposit.id) && minted.insert(deposit.id).second);
        assert(mint.output_index < (*tx)->vout.size() &&
               (*tx)->vout[mint.output_index] == *modern::MakeAssetOwnerOutput(g_asset, *amount, *recipient));
        block_sum += *amount;
        assert(block_sum <= Caps().max_per_block && epoch_before + block_sum <= Caps().max_per_epoch);
        assert(bridge::EpochAtSlot(base.light_client_slot) <= Pins().fork_schedule_valid_through_epoch);
        assert(Fresh(block.nTime, head.timestamp));
        keys.push_back(authorization.nullifier);
    }
    assert(delta.nullifiers_added == keys);
    if (block_sum > 0) {
        const std::vector expected{
            node::BridgeEpochMintChange{g_asset, epoch, epoch_before, epoch_before + block_sum}};
        assert(delta.epoch_mint_changes == expected);
    } else {
        assert(delta.epoch_mint_changes.empty());
    }

    // Each burn names its exact bUSD burn output and takes the next id.
    modern::WithdrawalTreeState tree{base.tree};
    assert(delta.withdrawal_tree_before == tree);
    std::set<node::BridgeWithdrawalId> sources;
    for (const node::BridgeDecentralizedWithdrawalRequest& request : delta.decentralized_withdrawals_added) {
        const auto tx{std::find_if(block.vtx.begin(), block.vtx.end(), [&](const CTransactionRef& candidate) {
            return candidate->GetHash() == request.transaction_id;
        })};
        assert(tx != block.vtx.end());
        const auto record{BridgeRecordOf(**tx)};
        assert(record && record->kind == bridge::BridgeRecordKindV1::BRIDGE_BURN);
        const auto& burn{std::get<bridge::BridgeBurnV1>(record->payload)};
        assert(burn.registry_id == g_registry && burn.burn_output_index == request.burn_output_index && !burn.exit);
        assert(burn.raw_amount >= 1 && burn.raw_amount <= static_cast<uint64_t>(MAX_MONEY));
        const CAmount amount{static_cast<CAmount>(burn.raw_amount)};
        assert(request.burn_output_index < (*tx)->vout.size() &&
               (*tx)->vout[request.burn_output_index] == *modern::MakeAssetBurnOutput(g_asset, amount));
        const modern::BridgeWithdrawalV1& withdrawal{request.withdrawal};
        assert(withdrawal.withdrawal_id == tree.count &&
               withdrawal.origin_chain_id == Bridge().asset.origin_chain_id && withdrawal.asset_id == g_asset &&
               withdrawal.origin_token == Bridge().asset.token_address &&
               withdrawal.recipient == burn.ethereum_recipient && !bridge::EthAddressIsNull(withdrawal.recipient) &&
               withdrawal.amount == amount && withdrawal.b3_height == static_cast<uint64_t>(height));
        assert(request.connected_height == height && request.connected_block == delta.block_hash);
        const node::BridgeWithdrawalId source{request.transaction_id, request.burn_output_index};
        assert(!base.burn_sources.contains(source) && sources.insert(source).second);
        const auto leaf{modern::BridgeWithdrawalLeafV1(withdrawal)};
        assert(leaf && *leaf == request.leaf && modern::AppendWithdrawalLeaf(tree, *leaf));
    }
    assert(delta.withdrawal_tree_after == tree);
}

//! VerifyTransaction is VerifyBlock of a one-transaction block.
void Harness::CheckTransactions(const std::vector<Planned>& planned, const int height, const uint32_t time,
                                const uint256& hash) const
{
    for (const Planned& p : planned) {
        node::BridgeTxAuthorization authorization;
        std::string error;
        const bool alone{m_index.VerifyTransaction(*p.tx, height, time, *g_params, authorization, error)};
        CBlock single;
        single.nTime = time;
        single.vtx = {p.tx};
        node::BridgeBlockDelta delta;
        assert(alone == m_index.VerifyBlock(single, height, hash, *g_params, delta, error));
        if (!alone) continue;
        assert(authorization.mint.has_value() == !delta.mint_authorizations.empty());
        if (authorization.mint) assert(*authorization.mint == delta.mint_authorizations.front());
        assert(authorization.decentralized_withdrawal.has_value() == !delta.decentralized_withdrawals_added.empty());
        if (authorization.decentralized_withdrawal) {
            // The preview names its own block.
            node::BridgeDecentralizedWithdrawalRequest request{*authorization.decentralized_withdrawal};
            request.connected_block = hash;
            assert(request == delta.decentralized_withdrawals_added.front());
        }
        assert(!authorization.withdrawal && !authorization.withdrawal_v2);
    }
}

//! Incremental preview, one transaction or a two-transaction chunk at a time,
//! accepts a chunk exactly when VerifyBlock accepts the chunks kept so far
//! plus that chunk; so it keeps every transaction of a block VerifyBlock
//! accepts, and a refused chunk leaves no trace.
void Harness::CheckPreview(const std::vector<Planned>& planned, const int height, const uint32_t time,
                           const uint256& hash, const bool accepted)
{
    std::string error;
    const auto preview{m_index.BeginBlockPreview(height, time, NextHash(), *g_params, error)};
    assert(preview);
    std::vector<CTransactionRef> kept;
    for (size_t i{0}; i < planned.size();) {
        const size_t chunk{i + 1 < planned.size() && m_fdp.ConsumeIntegralInRange<int>(0, 3) == 0 ? size_t{2} : size_t{1}};
        std::vector<CTransactionRef> txs;
        for (size_t j{i}; j < i + chunk; ++j) txs.push_back(planned[j].tx);
        const bool appended{preview->TryAppend(txs, error)};
        CBlock trial;
        trial.nTime = time;
        trial.vtx = kept;
        trial.vtx.insert(trial.vtx.end(), txs.begin(), txs.end());
        node::BridgeBlockDelta delta;
        assert(appended == m_index.VerifyBlock(trial, height, hash, *g_params, delta, error));
        if (appended) kept.insert(kept.end(), txs.begin(), txs.end());
        i += chunk;
    }
    assert(accepted == (kept.size() == planned.size()));
}

void Harness::Candidate()
{
    const int height{Tip().height + 1};
    const uint256 hash{NextHash()};
    const uint32_t time{ConsumeTime()};
    std::vector<Planned> planned;
    const size_t count{(m_fdp.ConsumeIntegralInRange<size_t>(0, 4) + 1) % 5};
    for (size_t i{0}; i < count; ++i) planned.push_back(MakeTransaction(planned));
    CBlock block;
    block.nTime = time;
    for (const Planned& p : planned) block.vtx.push_back(p.tx);

    node::BridgeBlockDelta delta;
    std::string error;
    const bool accepted{m_index.VerifyBlock(block, height, hash, *g_params, delta, error)};
    const Verdict verdict{Predict(planned, height, time, hash)};
    if (verdict.known) {
        assert(accepted == verdict.accept);
        if (accepted) assert(SameDelta(delta, verdict.delta));
    }
    CheckTransactions(planned, height, time, hash);
    CheckPreview(planned, height, time, hash, accepted);
    // Verifying, alone or incrementally, changed nothing.
    assert(TakeSnapshot(m_index) == Tip().snapshot);

    m_built.insert(m_built.end(), planned.begin(), planned.end());
    if (m_built.size() > 32) m_built.erase(m_built.begin(), m_built.end() - 32);
    if (!accepted) return;
    CheckSafety(block, delta);
    Connect(delta, std::move(block), {});
}

void Harness::AdvanceLightClient()
{
    // Up the canonical chain, or the same block at a later slot.
    const size_t current{Tip().model.light_client_block};
    const size_t block{current + m_fdp.ConsumeIntegralInRange<size_t>(
                                     0, std::min<size_t>(8, m_world.canonical - 1 - current))};
    const uint64_t slot{Tip().model.light_client_slot + m_fdp.ConsumeIntegralInRange<uint64_t>(1, 96)};
    const uint256 hash{NextHash()};
    Connect(LightClientDelta(m_index, Tip().height + 1, hash, block, slot), std::nullopt, {block, slot});
}

void Harness::Disconnect()
{
    if (m_chain.size() < 2) return; // the seed stays
    std::string error;
    // Only the exact tip disconnects.
    assert(!m_index.DisconnectBlock(Tip().height, NextHash(), error));
    assert(!m_index.DisconnectBlock(Tip().height - 1, m_chain[m_chain.size() - 2].hash, error));
    assert(TakeSnapshot(m_index) == Tip().snapshot);
    assert(m_index.DisconnectBlock(Tip().height, Tip().hash, error));
    m_detached = std::move(m_chain.back());
    m_chain.pop_back();
    assert(TakeSnapshot(m_index) == Tip().snapshot);
}

void Harness::Reconnect()
{
    if (!m_detached) return;
    Entry entry{std::move(*m_detached)};
    m_detached.reset();
    node::BridgeBlockDelta delta;
    std::string error;
    if (entry.block) {
        assert(m_index.VerifyBlock(*entry.block, entry.height, entry.hash, *g_params, delta, error));
    } else {
        delta = LightClientDelta(m_index, entry.height, entry.hash, entry.light_client.first,
                                 entry.light_client.second);
    }
    assert(SameDelta(delta, entry.delta));
    assert(m_index.ConnectBlock(delta, error));
    assert(TakeSnapshot(m_index) == entry.snapshot);
    m_chain.push_back(std::move(entry));
}

//! The index answers as the model of the active chain.
void Harness::CheckIndex() const
{
    const Entry& tip{Tip()};
    const Model& model{tip.model};
    assert(m_index.ConnectedHeight() == tip.height && m_index.ConnectedHash() == tip.hash);
    const ExecBlock& head{m_world.blocks[model.light_client_block]};
    const auto& store{m_index.LightClient()};
    assert(store && store->finalized_header.beacon.slot == model.light_client_slot &&
           store->finalized_header.execution.block_hash == head.hash &&
           store->finalized_header.execution.block_number == head.number &&
           store->finalized_header.execution.receipts_root == head.receipts_root &&
           store->finalized_header.execution.timestamp == head.timestamp);
    assert(m_index.LightClientLastConnected() == model.light_client_connection);

    assert(m_index.AnchorCount() == model.anchors.size());
    for (const ExecBlock& block : m_world.blocks) {
        const auto found{model.anchors.find(block.hash)};
        assert(m_index.Anchor(block.hash) == (found == model.anchors.end()
                                                  ? std::nullopt
                                                  : std::optional<node::BridgeExecutionAnchor>{found->second}));
    }
    assert(m_index.NullifierCount() == model.minted.size() &&
           m_index.NullifierCount(bridge::BRIDGE_NULLIFIER_NAMESPACE_V1) == model.minted.size());
    for (const Deposit& deposit : m_world.deposits) {
        const auto found{model.minted.find(deposit.id)};
        assert(m_index.IsNullified(Key(deposit.id)) == (found != model.minted.end()));
        assert(m_index.NullifierConnection(Key(deposit.id)) ==
               (found == model.minted.end() ? std::nullopt
                                            : std::optional<node::BridgeStateConnection>{found->second}));
    }
    for (const uint64_t epoch : Epochs()) assert(m_index.EpochMinted(g_asset, epoch) == model.EpochMinted(epoch));

    assert(m_index.WithdrawalCount() == 0);
    assert(m_index.WithdrawalTree() == model.tree);
    assert(m_index.DecentralizedWithdrawalCount() == model.withdrawals.size());
    for (size_t id{0}; id < model.withdrawals.size(); ++id) {
        const node::BridgeDecentralizedWithdrawalRequest& request{model.withdrawals[id]};
        assert(m_index.DecentralizedWithdrawal(uint64_t{id}) == request);
        assert(m_index.DecentralizedWithdrawal(
                   node::BridgeWithdrawalId{request.transaction_id, request.burn_output_index}) == request);
    }
    assert(!m_index.DecentralizedWithdrawal(uint64_t{model.withdrawals.size()}));
    for (const Entry& entry : m_chain) {
        assert(m_index.WithdrawalRootAtHeight(entry.height) == model.roots.at(entry.height));
        std::vector<node::BridgeDecentralizedWithdrawalRequest> through;
        for (const auto& request : model.withdrawals) {
            if (request.connected_height <= entry.height) through.push_back(request);
        }
        assert(m_index.DecentralizedWithdrawalsThrough(entry.height) == through);
    }
    assert(!m_index.WithdrawalRootAtHeight(tip.height + 1));

    const auto& history{m_index.History()};
    assert(history.size() == m_chain.size());
    for (size_t i{0}; i < history.size(); ++i) assert(SameDelta(history[i], m_chain[i].delta));
}

//! A fresh index replaying the active chain derives the same deltas and
//! ends in the same state.
void Harness::CheckReplay() const
{
    node::BridgeStateIndex fresh;
    for (const Entry& entry : m_chain) {
        node::BridgeBlockDelta delta;
        std::string error;
        if (entry.block) {
            assert(fresh.VerifyBlock(*entry.block, entry.height, entry.hash, *g_params, delta, error));
        } else {
            delta = LightClientDelta(fresh, entry.height, entry.hash, entry.light_client.first,
                                     entry.light_client.second);
        }
        assert(SameDelta(delta, entry.delta));
        assert(fresh.ConnectBlock(delta, error));
    }
    assert(TakeSnapshot(fresh) == TakeSnapshot(m_index));
}

} // namespace

FUZZ_TARGET(bridge_state, .init = initialize_bridge_state)
{
    FuzzedDataProvider fdp{buffer.data(), buffer.size()};
    const World world{BuildWorld(fdp)};
    Harness harness{world, fdp};
    LIMITED_WHILE(fdp.ConsumeBool(), MAX_STEPS)
    {
        switch (fdp.ConsumeIntegralInRange<int>(0, 6)) {
        case 0:
        case 1:
        case 2:
        case 3: harness.Candidate(); break;
        case 4: harness.AdvanceLightClient(); break;
        case 5: harness.Disconnect(); break;
        default: harness.Reconnect(); break;
        }
        harness.CheckIndex();
    }
    harness.CheckReplay();
}
