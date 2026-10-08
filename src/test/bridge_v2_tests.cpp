// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Bridge V2 pure layer: the BridgeV2Params envelope and its activation
// predicates (consensus/bridge_params.h, consensus/params.h), the bUSD V2
// AssetId, vault entry id and registry id V2 (modern/bridge_v2.h), leaf V2
// and the V2 withdrawal tree (modern/withdrawal_v2.h), and the
// B3VaultV2.release calldata (bridge/withdrawal_v2_calldata.h). Identities
// follow doc/design/b3-bridge-v2-node-changes.md section 3.2; leaf V2 bytes
// are owned by doc/design/b3-bridge-v2-contracts-spec.md section 6.4. The
// B-23 successor entry, its identities and migration predicates, and the
// MIGRATE (sweep) leaf follow (node changes section 7.6). The
// V2 deposit leg's receipt-proof limits and EIP-2718 receipt types
// (bridge/proof.h, bridge/deposit.h; node changes section 4), the nullifier
// namespace (bridge/admission.h) and V2 admission (bridge/admission_v2.h),
// by vault entry for the B-23 successor too, follow the identity cases. The fresh-lineage withdrawal-root hook
// (node/finality_fresh_lineage.h; node changes section 7.4) closes the file.
//
// Vector provenance. The asset_id_v2, leaf_v2 and withdrawal_tree_v2 values
// and the tag hashes below equal the shared C++/Foundry vectors file
// contracts/test/vectors/bridge_v2_vectors.json, written by
// contracts/test/vectors/gen_v2_vectors.sh ("tags", "asset_id_v2[0]",
// "leaf_v2[0]", "withdrawal_tree_v2", and "history.roots[0]" for the empty
// depth-32 root; "finalized_block_v2[1].withdrawal_root", the lineage-2
// block at history.bridge_activation_height, is that tree's root as well).
// This test hard-codes those values rather than reading that
// file, so a regenerated file must be copied here again. Hex is in EVM byte
// order: the 32 bytes of a hash are its raw uint256 bytes. The release
// selector is `cast sig "release((uint64,address,uint256,uint64),bytes32[32])"`.

#include <bridge/admission.h>
#include <bridge/admission_v2.h>
#include <bridge/deposit.h>
#include <bridge/proof.h>
#include <bridge/reserve_v2.h>
#include <bridge/rlp.h>
#include <bridge/withdrawal_v2_calldata.h>
#include <chainparams.h>
#include <consensus/amount.h>
#include <consensus/bridge_params.h>
#include <consensus/params.h>
#include <crypto/sha256.h>
#include <kernel/chainparams.h>
#include <modern/bridge_asset.h>
#include <modern/bridge_forwarder.h>
#include <modern/bridge_v2.h>
#include <modern/chain_domain.h>
#include <modern/withdrawal_tree.h>
#include <modern/withdrawal_v2.h>
#include <node/bridge_state.h>
#include <node/finality_fresh_lineage.h>
#include <primitives/transaction_identifier.h>
#include <streams.h>
#include <test/util/bridge_state_access.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

//! A hash whose 32 raw bytes are the given EVM-order hex (no byte reversal).
uint256 Evm(const std::string_view hex)
{
    const auto bytes{ParseHex(hex)};
    BOOST_REQUIRE_EQUAL(bytes.size(), 32U);
    uint256 out;
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return out;
}

//! EVM-order hex of a hash's raw bytes.
std::string EvmHex(const uint256& h) { return HexStr(std::span<const unsigned char>(h.begin(), 32)); }

Consensus::BridgeEthAddress Address(const std::string_view hex)
{
    const auto bytes{ParseHex(hex)};
    BOOST_REQUIRE_EQUAL(bytes.size(), 20U);
    Consensus::BridgeEthAddress out{};
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return out;
}

Consensus::BridgeEthAddress FillAddress(const unsigned char b)
{
    Consensus::BridgeEthAddress out{};
    out.fill(b);
    return out;
}

uint256 Fill(const unsigned char b)
{
    uint256 out;
    std::fill(out.begin(), out.end(), b);
    return out;
}

uint256 Sha256Of(const std::string_view text)
{
    uint256 out;
    CSHA256()
        .Write(reinterpret_cast<const unsigned char*>(text.data()), text.size())
        .Finalize(out.begin());
    return out;
}

// --------------------------------------------------------- shared vectors

const std::string_view MAINNET_CHAIN_DOMAIN{"a651262ae9031646047f290513d77ccae0fa9bd4e5afe7e07155a08d5dd1486a"};
const std::string_view STAND_IN_VAULT{"2222222222222222222222222222222222222222"};
const std::string_view USDT{"dac17f958d2ee523a2206206994597c13d831ec7"};
const std::string_view STAND_IN_RECIPIENT{"3333333333333333333333333333333333333333"};

const std::string_view TAG_ASSET_V2{"7bf39d27dc845091cf66adcf93912e3ffeaed119a38c3d5bf4479821a90e16ce"};
const std::string_view TAG_LEAF_V2{"c250a4de311e4369a5d75a6fdd6351f1cc8ec35929170d1374a5367a5edd10f5"};

const std::string_view ASSET_ID_V2_PREIMAGE{
    "7bf39d27dc845091cf66adcf93912e3ffeaed119a38c3d5bf4479821a90e16ce"
    "7bf39d27dc845091cf66adcf93912e3ffeaed119a38c3d5bf4479821a90e16ce"
    "a651262ae9031646047f290513d77ccae0fa9bd4e5afe7e07155a08d5dd1486a"
    "02"
    "0100000000000000"
    "2222222222222222222222222222222222222222"
    "dac17f958d2ee523a2206206994597c13d831ec7"
    "0606"};
const std::string_view ASSET_ID_V2{"db9e32397f3656fb2317c7829429f430cfefcf665c08a26774cb7b2fb3c76fcd"};

const std::string_view LEAF_V2_PREIMAGE{
    "c250a4de311e4369a5d75a6fdd6351f1cc8ec35929170d1374a5367a5edd10f5"
    "1111111111111111111111111111111111111111111111111111111111111111"
    "0000000000000007"
    "0000000000000001"
    "2222222222222222222222222222222222222222"
    "dac17f958d2ee523a2206206994597c13d831ec7"
    "3333333333333333333333333333333333333333"
    "00000000000000000000000000000000000000000000000000000000000f4240"
    "00000000000dbba0"};
const std::string_view LEAF_V2{"1cb905899ca0c488e158585c0208497d5415d1645035fbca3ca98ee2efb5ad64"};

const std::array<std::string_view, 8> TREE_V2_LEAVES{
    "80fbd0eadc11a49019b9838d13e557acf3523d92cebacdec9011accc2f437cdd",
    "d42558125c6f4bc19bd6a8d134b26864e6a28515e1fe43f4ddaa7d1a3d82e8bd",
    "d6d2b8f9818df8c2a54b3681eaeb68d948d711390d0283d0aabf6df7e6ee7a49",
    "76ebbc2128ea0465a5c0eff3cc46da430e3b06eda687f40e18886c29468714fe",
    "f6485c529b44c76459b855f1e14711832d6af5a3aa6d24e68e5b2ec8e42f49c2",
    "485c4a0da7e4f868002a4c4d463a0be7d65a438666b6ed45e6948c0168b1d535",
    "ac9e91bec6352744295ac1dd130c508479ff1352f40a6adf75071b53ae3346d9",
    "1cb905899ca0c488e158585c0208497d5415d1645035fbca3ca98ee2efb5ad64",
};
const std::string_view TREE_V2_ROOT{"e2b107af8daa890eec0fef52f0f558733348e1cadf982fb8eec04124eb44d79a"};
const std::string_view EMPTY_TREE_ROOT{"27ae5ba08d7291c96c8cbddcc148bf48a6d68c7974b94356f53754ef6171d757"};

//! Siblings [0,3) of paths[index 0] and paths[index 7].
const std::array<std::string_view, 3> TREE_V2_PATH0_LOW{
    "d42558125c6f4bc19bd6a8d134b26864e6a28515e1fe43f4ddaa7d1a3d82e8bd",
    "e1d037899c7a893587aa71c979df49cb311fb3ca086f9d8a391747ac0b7c87cb",
    "39d4a2ecc035bad540c28efe70689771f3f61a145d11bf70eb88cd7ad6b3bfbd",
};
const std::array<std::string_view, 3> TREE_V2_PATH7_LOW{
    "ac9e91bec6352744295ac1dd130c508479ff1352f40a6adf75071b53ae3346d9",
    "0170d60d9b1cbf3a04c852c2f15b566df3b7c2283cee00af913e53d347fd6f77",
    "5a8b0ede2f8c4e5f763e3e5c7f2e8a376635f21a8e2d094c922f5f853d60073b",
};
//! Siblings [3,32), identical in both paths of an eight-leaf tree.
const std::array<std::string_view, 29> TREE_V2_PATH_HIGH{
    "21ddb9a356815c3fac1026b6dec5df3124afbadb485c9ba5a3e3398a04b7ba85",
    "e58769b32a1beaf1ea27375a44095a0d1fb664ce2dd358e7fcbfb78c26a19344",
    "0eb01ebfc9ed27500cd4dfc979272d1f0913cc9f66540d7e8005811109e1cf2d",
    "887c22bd8750d34016ac3c66b5ff102dacdd73f6b014e710b51e8022af9a1968",
    "ffd70157e48063fc33c97a050f7f640233bf646cc98d9524c6b92bcf3ab56f83",
    "9867cc5f7f196b93bae1e27e6320742445d290f2263827498b54fec539f756af",
    "cefad4e508c098b9a7e1d8feb19955fb02ba9675585078710969d3440f5054e0",
    "f9dc3e7fe016e050eff260334f18a5d4fe391d82092319f5964f2e2eb7c1c3a5",
    "f8b13a49e282f609c317a833fb8d976d11517c571d1221a265d25af778ecf892",
    "3490c6ceeb450aecdc82e28293031d10c7d73bf85e57bf041a97360aa2c5d99c",
    "c1df82d9c4b87413eae2ef048f94b4d3554cea73d92b0f7af96e0271c691e2bb",
    "5c67add7c6caf302256adedf7ab114da0acfe870d449a3a489f781d659e8becc",
    "da7bce9f4e8618b6bd2f4132ce798cdc7a60e7e1460a7299e3c6342a579626d2",
    "2733e50f526ec2fa19a22b31e8ed50f23cd1fdf94c9154ed3a7609a2f1ff981f",
    "e1d3b5c807b281e4683cc6d6315cf95b9ade8641defcb32372f1c126e398ef7a",
    "5a2dce0a8a7f68bb74560f8f71837c2c2ebbcbf7fffb42ae1896f13f7c7479a0",
    "b46a28b6f55540f89444f63de0378e3d121be09e06cc9ded1c20e65876d36aa0",
    "c65e9645644786b620e2dd2ad648ddfcbf4a7e5b1a3a4ecfe7f64667a3f0b7e2",
    "f4418588ed35a2458cffeb39b93d26f18d2ab13bdce6aee58e7b99359ec2dfd9",
    "5a9c16dc00d6ef18b7933a6f8dc65ccb55667138776f7dea101070dc8796e377",
    "4df84f40ae0c8229d0d6069e5c8f39a7c299677a09d367fc7b05e3bc380ee652",
    "cdc72595f74c7b1043d0e1ffbab734648c838dfb0527d971b602bc216c9619ef",
    "0abf5ac974a1ed57f4050aa510dd9c74f508277b39d7973bb2dfccc5eeb0618d",
    "b8cd74046ff337f0a7bf2c8e03e10f642c1886798d71806ab1e888d9e5ee87d0",
    "838c5655cb21c6cb83313b5a631175dff4963772cce9108188b34ac87c81c41e",
    "662ee4dd2dd7b2bc707961b1e646c4047669dcb6584f0d8d770daf5d7e7deb2e",
    "388ab20e2573d171a88108e79d820e98f26c0b84aa8b2f4aa4968dbb818ea322",
    "93237c50ba75ee485f4c22adf2f741400bdf8d6a9cc7df7ecae576221665d735",
    "8448818bb4ae4562849e949e17ac16e0be16688e156b5cf15e098c627c0056a9",
};

std::array<uint256, modern::WITHDRAWAL_TREE_DEPTH> VectorPath(const std::array<std::string_view, 3>& low)
{
    std::array<uint256, modern::WITHDRAWAL_TREE_DEPTH> out{};
    for (size_t i{0}; i < low.size(); ++i) out[i] = Evm(low[i]);
    for (size_t i{0}; i < TREE_V2_PATH_HIGH.size(); ++i) out[3 + i] = Evm(TREE_V2_PATH_HIGH[i]);
    return out;
}

//! The vector's withdrawal with the given id (leaf_v2 is id 7 of this set).
modern::BridgeWithdrawalV2 VectorWithdrawal(const uint64_t id)
{
    modern::BridgeWithdrawalV2 w;
    w.withdrawal_id = id;
    w.destination_chain_id = 1;
    w.asset_id = Fill(0x11);
    w.vault = Address(STAND_IN_VAULT);
    w.token = Address(USDT);
    w.recipient = Address(STAND_IN_RECIPIENT);
    w.amount = 1'000'000;
    w.b3_height = 900'000;
    return w;
}

std::vector<modern::BridgeWithdrawalV2> VectorPrefix()
{
    std::vector<modern::BridgeWithdrawalV2> out;
    for (uint64_t id{0}; id < TREE_V2_LEAVES.size(); ++id) out.push_back(VectorWithdrawal(id));
    return out;
}

// ------------------------------------------------------- params builders

Consensus::EthereumLightClientPins LightClient(const uint256& gvr)
{
    Consensus::EthereumLightClientPins light;
    light.trusted_checkpoint_root = Fill(0x08);
    light.trusted_checkpoint_slot = 8'192;
    light.genesis_validators_root = gvr;
    light.fork_schedule = {
        {0, {0x00, 0x00, 0x00, 0xb3}},
        {10, {0x01, 0x00, 0x00, 0xb3}},
    };
    light.fork_schedule_valid_through_epoch = 1'024;
    light.electra_epoch = 10;
    light.min_sync_committee_participants = Consensus::ETHEREUM_SYNC_COMMITTEE_SUPERMAJORITY;
    light.max_sync_lag_slots = 65'536;
    return light;
}

//! A complete regtest-shaped envelope: anvil chain 31337, OI-3 hashes unset.
Consensus::BridgeV2Params RegtestV2Params()
{
    Consensus::BridgeV2Params out;
    out.activation_height = 400;
    out.burn_height = 400;
    out.light_client = LightClient(Fill(0x09));
    out.verifier.address = FillAddress(0x66);
    out.verifier.min_bridge_validators = 4;
    out.verifier.max_bridge_validators = 64;
    out.verifier.min_bridge_total_weight = 4;
    out.vault.origin_chain_id = 31'337;
    out.vault.vault_address = FillAddress(0x44);
    out.vault.token_address = FillAddress(0x55);
    out.vault.token_runtime_code_hash = Fill(0x0a);
    out.vault.origin_deployment_block = 3;
    out.vault.max_deposit_raw = Consensus::BRIDGE_V2_DEFAULT_CAP_RAW;
    out.vault.mint_caps = Consensus::BridgeMintCaps{
        .max_per_block = Consensus::BRIDGE_V2_DEFAULT_CAP_RAW,
        .max_per_epoch = Consensus::BRIDGE_V2_DEFAULT_CAP_RAW,
        .epoch_length_blocks = 1'440,
    };
    return out;
}

//! A complete Ethereum-mainnet-shaped envelope (every pin set).
Consensus::BridgeV2Params MainnetShapedV2Params()
{
    Consensus::BridgeV2Params out{RegtestV2Params()};
    out.light_client = LightClient(Consensus::ETHEREUM_MAINNET_GENESIS_VALIDATORS_ROOT);
    out.vault.origin_chain_id = Consensus::BUSD_ETHEREUM_CHAIN_ID;
    out.vault.token_address = Consensus::BUSD_ETHEREUM_USDT;
    out.vault.token_runtime_code_hash = Consensus::BUSD_ETHEREUM_USDT_RUNTIME_CODE_HASH;
    out.vault.vault_runtime_code_hash = Fill(0x0b);
    out.verifier.runtime_code_hash = Fill(0x0c);
    out.verifier.min_bridge_total_weight = 900;
    out.verifier.max_epoch_lag_seconds = out.gate.set_expiry_seconds;
    return out;
}

Consensus::Params ConsensusWith(const std::optional<Consensus::BridgeV2Params>& v2)
{
    Consensus::Params out;
    out.hashGenesisBlock = Fill(0x01);
    out.legacy_final_hash = Fill(0x02);
    out.bridge_v2 = v2;
    return out;
}

//! The modern chain domain of every ConsensusWith() envelope.
uint256 FixtureDomain()
{
    const Consensus::Params params{ConsensusWith(std::nullopt)};
    const auto domain{modern::ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash)};
    BOOST_REQUIRE(domain);
    return *domain;
}

//! `p` with a valid B-23 successor under `domain` (node changes section
//! 7.6): vault 0x46..46 on the launch entry's chain, token and caps, its own
//! deployment block, its vault code hash unset (OI-3, as the regtest pin file
//! leaves it), predecessor v, v's AssetId and h_m = V + 60.
Consensus::BridgeV2Params WithSuccessor(Consensus::BridgeV2Params p, const uint256& domain)
{
    Consensus::BridgeV2SuccessorEntry s;
    s.entry = p.vault;
    s.entry.vault_address = FillAddress(0x46);
    s.entry.vault_runtime_code_hash.reset();
    s.entry.origin_deployment_block = p.vault.origin_deployment_block + 10;
    s.predecessor = p.vault.vault_address;
    s.asset_id = modern::BridgeAssetIdV2(domain, p.vault);
    s.migration_height = p.activation_height + 60;
    p.successor = s;
    return p;
}

} // namespace

BOOST_AUTO_TEST_SUITE(bridge_v2_tests)

BOOST_AUTO_TEST_CASE(asset_id_v2_matches_shared_vector)
{
    BOOST_CHECK_EQUAL(EvmHex(Sha256Of(modern::BRIDGE_ASSET_V2_TAG)), TAG_ASSET_V2);

    const uint256 domain{Evm(MAINNET_CHAIN_DOMAIN)};
    Consensus::BridgeV2VaultEntry entry;
    entry.origin_chain_id = 1;
    entry.vault_address = Address(STAND_IN_VAULT);
    entry.token_address = Address(USDT);
    entry.origin_decimals = 6;
    entry.asset_decimals = 6;

    // The 147-byte preimage is sha256(tag) twice, then the serialized message.
    DataStream message{};
    modern::SerializeBridgeAssetIdV2Message(message, domain, entry);
    BOOST_REQUIRE_EQUAL(message.size(), modern::BRIDGE_ASSET_ID_V2_MESSAGE_SIZE);
    const std::string message_hex{HexStr(std::span<const std::byte>{message.data(), message.size()})};
    const std::string tag_hex{EvmHex(Sha256Of(modern::BRIDGE_ASSET_V2_TAG))};
    BOOST_CHECK_EQUAL(tag_hex + tag_hex + message_hex, ASSET_ID_V2_PREIMAGE);

    const modern::AssetId asset{modern::BridgeAssetIdV2(domain, entry)};
    BOOST_CHECK_EQUAL(EvmHex(asset), ASSET_ID_V2);

    // The id is vault-bound only: code hashes, caps and the deployment block
    // are registry pins and never rename the asset.
    Consensus::BridgeV2VaultEntry pinned{entry};
    pinned.vault_runtime_code_hash = Fill(0x0b);
    pinned.token_runtime_code_hash = Fill(0x0a);
    pinned.origin_deployment_block = 99;
    pinned.max_deposit_raw = 7;
    BOOST_CHECK(modern::BridgeAssetIdV2(domain, pinned) == asset);
    Consensus::BridgeV2VaultEntry other_vault{entry};
    other_vault.vault_address = FillAddress(0x23);
    BOOST_CHECK(modern::BridgeAssetIdV2(domain, other_vault) != asset);
    BOOST_CHECK(modern::BridgeAssetIdV2(Fill(0x01), entry) != asset);

    // Tag and version separate it from the V1 id of the same tuple.
    Consensus::BridgeAssetIdentityV1 v1;
    v1.origin_chain_id = 1;
    v1.vault_address = entry.vault_address;
    v1.token_address = entry.token_address;
    v1.origin_decimals = 6;
    v1.asset_decimals = 6;
    BOOST_CHECK(modern::BridgeAssetIdV1(domain, v1) != asset);

    // The vector's domain is B3 mainnet's modern chain domain.
    const auto mainnet{CChainParams::Main()};
    const Consensus::Params& consensus{mainnet->GetConsensus()};
    BOOST_REQUIRE(consensus.legacy_final_hash);
    const auto main_domain{modern::ModernChainDomain(consensus.hashGenesisBlock, *consensus.legacy_final_hash)};
    BOOST_REQUIRE(main_domain);
    BOOST_CHECK_EQUAL(EvmHex(*main_domain), MAINNET_CHAIN_DOMAIN);
}

BOOST_AUTO_TEST_CASE(leaf_v2_matches_shared_vector)
{
    BOOST_CHECK_EQUAL(EvmHex(modern::WithdrawalLeafTagV2()), TAG_LEAF_V2);

    const modern::BridgeWithdrawalV2 w{VectorWithdrawal(7)};
    const auto preimage{modern::EncodeBridgeWithdrawalV2(w)};
    BOOST_REQUIRE(preimage);
    BOOST_CHECK_EQUAL(preimage->size(), 180U);
    BOOST_CHECK_EQUAL(HexStr(*preimage), LEAF_V2_PREIMAGE);
    const auto leaf{modern::BridgeWithdrawalLeafV2(w)};
    BOOST_REQUIRE(leaf);
    BOOST_CHECK_EQUAL(EvmHex(*leaf), LEAF_V2);

    // Boundaries that stay encodable.
    auto edge{w};
    edge.withdrawal_id = modern::MAX_WITHDRAWAL_LEAVES - 1;
    edge.amount = MAX_MONEY;
    const auto edge_bytes{modern::EncodeBridgeWithdrawalV2(edge)};
    BOOST_REQUIRE(edge_bytes);
    BOOST_CHECK_EQUAL(HexStr(std::span<const unsigned char>{*edge_bytes}.subspan(64, 8)), "00000000ffffffff");
    BOOST_CHECK_EQUAL(HexStr(std::span<const unsigned char>{*edge_bytes}.subspan(140, 24)), std::string(48, '0'));

    // Every value Vault V2 could never release is refused.
    const std::vector<std::function<void(modern::BridgeWithdrawalV2&)>> refusals{
        [](auto& x) { x.withdrawal_id = modern::MAX_WITHDRAWAL_LEAVES; },
        [](auto& x) { x.destination_chain_id = 0; },
        [](auto& x) { x.asset_id = uint256{}; },
        [](auto& x) { x.vault = Consensus::BridgeEthAddress{}; },
        [](auto& x) { x.token = Consensus::BridgeEthAddress{}; },
        [](auto& x) { x.recipient = Consensus::BridgeEthAddress{}; },
        [](auto& x) { x.amount = 0; },
        [](auto& x) { x.amount = -1; },
        [](auto& x) { x.amount = MAX_MONEY + 1; },
    };
    for (const auto& mutate : refusals) {
        auto bad{w};
        mutate(bad);
        BOOST_CHECK(!modern::EncodeBridgeWithdrawalV2(bad));
        BOOST_CHECK(!modern::BridgeWithdrawalLeafV2(bad));
    }

    // A V2 leaf never equals the V1 leaf of the overlapping fields.
    modern::BridgeWithdrawalV1 v1;
    v1.withdrawal_id = w.withdrawal_id;
    v1.origin_chain_id = w.destination_chain_id;
    v1.asset_id = w.asset_id;
    v1.origin_token = w.token;
    v1.recipient = w.recipient;
    v1.amount = w.amount;
    v1.b3_height = w.b3_height;
    BOOST_CHECK(modern::BridgeWithdrawalLeafV1(v1) != leaf);
}

BOOST_AUTO_TEST_CASE(withdrawal_tree_v2_root_and_paths)
{
    // Empty V2 tree: the non-zero depth-32 zero-subtree root (history.roots[0]).
    const modern::WithdrawalTreeState empty;
    BOOST_CHECK_EQUAL(empty.count, 0U);
    BOOST_CHECK(empty.root == modern::WithdrawalZeroHashes()[modern::WITHDRAWAL_TREE_DEPTH]);
    BOOST_CHECK(!empty.root.IsNull());
    BOOST_CHECK_EQUAL(EvmHex(empty.root), EMPTY_TREE_ROOT);

    const auto prefix{VectorPrefix()};
    modern::WithdrawalTreeState tree;
    for (size_t i{0}; i < prefix.size(); ++i) {
        // An id other than the next index is refused and changes nothing.
        auto skipped{prefix[i]};
        skipped.withdrawal_id = i + 1;
        const auto before{tree};
        BOOST_CHECK(!modern::AppendBridgeWithdrawalV2(tree, skipped));
        BOOST_CHECK(tree == before);

        const auto leaf{modern::AppendBridgeWithdrawalV2(tree, prefix[i])};
        BOOST_REQUIRE(leaf);
        BOOST_CHECK_EQUAL(EvmHex(*leaf), TREE_V2_LEAVES[i]);
    }
    BOOST_CHECK_EQUAL(tree.count, 8U);
    BOOST_CHECK_EQUAL(EvmHex(tree.root), TREE_V2_ROOT);
    // An invalid withdrawal at the right index is refused too.
    auto invalid{VectorWithdrawal(8)};
    invalid.amount = 0;
    BOOST_CHECK(!modern::AppendBridgeWithdrawalV2(tree, invalid));
    BOOST_CHECK_EQUAL(tree.count, 8U);

    std::string error;
    const auto zero{bridge::BuildWithdrawalV2RelayArtifacts(prefix, 0, tree.root, error)};
    BOOST_REQUIRE_MESSAGE(zero, error);
    BOOST_CHECK(zero->path == VectorPath(TREE_V2_PATH0_LOW));
    BOOST_CHECK_EQUAL(EvmHex(zero->leaf), TREE_V2_LEAVES[0]);
    const auto seven{bridge::BuildWithdrawalV2RelayArtifacts(prefix, 7, tree.root, error)};
    BOOST_REQUIRE_MESSAGE(seven, error);
    BOOST_CHECK(seven->path == VectorPath(TREE_V2_PATH7_LOW));
    BOOST_CHECK_EQUAL(EvmHex(seven->leaf), LEAF_V2);
    BOOST_CHECK_EQUAL(HexStr(seven->leaf_preimage), LEAF_V2_PREIMAGE);
    BOOST_CHECK(seven->withdrawal == prefix[7]);
    BOOST_CHECK(seven->root == tree.root);
    // The high siblings are the zero-subtree hashes.
    for (unsigned d{3}; d < modern::WITHDRAWAL_TREE_DEPTH; ++d) {
        BOOST_CHECK(seven->path[d] == modern::WithdrawalZeroHashes()[d]);
    }

    // A one-leaf prefix proves against its own (different) root.
    modern::WithdrawalTreeState one;
    BOOST_REQUIRE(modern::AppendBridgeWithdrawalV2(one, prefix[0]));
    const auto single{bridge::BuildWithdrawalV2RelayArtifacts(std::span{prefix}.first(1), 0, one.root, error)};
    BOOST_REQUIRE_MESSAGE(single, error);
    BOOST_CHECK(single->root != tree.root);

    // Refusals.
    BOOST_CHECK(!bridge::BuildWithdrawalV2RelayArtifacts(prefix, 8, tree.root, error));
    BOOST_CHECK_EQUAL(error, "withdrawal is not present in the finalized prefix");
    BOOST_CHECK(!bridge::BuildWithdrawalV2RelayArtifacts(prefix, 0, one.root, error));
    BOOST_CHECK_EQUAL(error, "withdrawal prefix does not reproduce the finalized root");
    auto gap{prefix};
    gap[3].withdrawal_id = 4;
    BOOST_CHECK(!bridge::BuildWithdrawalV2RelayArtifacts(gap, 0, tree.root, error));
    BOOST_CHECK_EQUAL(error, "withdrawal prefix is not consecutive");
    auto bad{prefix};
    bad[5].recipient = Consensus::BridgeEthAddress{};
    BOOST_CHECK(!bridge::BuildWithdrawalV2RelayArtifacts(bad, 0, tree.root, error));
    BOOST_CHECK_EQUAL(error, "withdrawal prefix contains an invalid leaf");
}

BOOST_AUTO_TEST_CASE(release_v2_calldata_layout)
{
    const auto prefix{VectorPrefix()};
    modern::WithdrawalTreeState tree;
    for (const auto& w : prefix) BOOST_REQUIRE(modern::AppendBridgeWithdrawalV2(tree, w));
    std::string error;
    const auto artifacts{bridge::BuildWithdrawalV2RelayArtifacts(prefix, 7, tree.root, error)};
    BOOST_REQUIRE_MESSAGE(artifacts, error);
    BOOST_CHECK(error.empty());

    const std::span<const unsigned char> calldata{artifacts->release_calldata};
    BOOST_REQUIRE_EQUAL(calldata.size(), bridge::VAULT_V2_RELEASE_CALLDATA_SIZE);
    BOOST_CHECK_EQUAL(calldata.size(), 1156U);
    // cast sig / Solidity B3VaultV2.release.selector (same tuple as V1).
    BOOST_CHECK_EQUAL(HexStr(calldata.first(4)), "ca514def");
    auto word{[&](const size_t i) { return HexStr(calldata.subspan(4 + 32 * i, 32)); }};
    BOOST_CHECK_EQUAL(word(0), "0000000000000000000000000000000000000000000000000000000000000007");
    BOOST_CHECK_EQUAL(word(1), "000000000000000000000000" + std::string{STAND_IN_RECIPIENT});
    BOOST_CHECK_EQUAL(word(2), "00000000000000000000000000000000000000000000000000000000000f4240");
    BOOST_CHECK_EQUAL(word(3), "00000000000000000000000000000000000000000000000000000000000dbba0");
    const auto expected_path{VectorPath(TREE_V2_PATH7_LOW)};
    for (size_t d{0}; d < modern::WITHDRAWAL_TREE_DEPTH; ++d) {
        BOOST_CHECK_EQUAL(word(4 + d), EvmHex(expected_path[d]));
    }
    // The vault folds the leaf up these words to the certified root.
    BOOST_CHECK(modern::FoldFinalityHistoryPath(artifacts->leaf, 7, artifacts->path) == tree.root);
}

BOOST_AUTO_TEST_CASE(bridge_v2_params_valid_refusals)
{
    BOOST_CHECK(RegtestV2Params().Valid());
    BOOST_CHECK(MainnetShapedV2Params().Valid());
    BOOST_CHECK(!Consensus::BridgeV2Params{}.Valid());

    using P = Consensus::BridgeV2Params;
    // Mutations that keep the envelope valid.
    const std::vector<std::function<void(P&)>> accepted{
        [](P& p) { p.burn_height = p.activation_height + 5; },
        [](P& p) { p.activation_height = 1; p.burn_height = 1; },
        [](P& p) { p.vault.vault_runtime_code_hash = Fill(0x0b); },
        [](P& p) { p.verifier.runtime_code_hash = Fill(0x0c); },
        [](P& p) { p.vault.max_deposit_raw = 1; },
        [](P& p) { p.vault.mint_caps.max_per_epoch = 2 * p.vault.mint_caps.max_per_block; },
        [](P& p) { p.verifier.min_bridge_validators = 64; },
        [](P& p) { p.verifier.max_bridge_validators = 4; },
        // The verifier's MAX_EPOCH_LAG mirror equal to the gate's set expiry.
        [](P& p) { p.verifier.max_epoch_lag_seconds = p.gate.set_expiry_seconds; },
    };
    for (const auto& mutate : accepted) {
        P p{RegtestV2Params()};
        mutate(p);
        BOOST_CHECK(p.Valid());
    }

    // One mutation per Valid() rule.
    const std::vector<std::function<void(P&)>> refused{
        // V and W2
        [](P& p) { p.activation_height = 0; p.burn_height = 0; },
        [](P& p) { p.activation_height = -1; },
        [](P& p) { p.burn_height = p.activation_height - 1; },
        // light client
        [](P& p) { p.light_client.trusted_checkpoint_root = uint256{}; },
        [](P& p) { p.light_client.fork_schedule.clear(); },
        // vault entry
        [](P& p) { p.vault.origin_chain_id = 0; },
        [](P& p) { p.vault.vault_address = Consensus::BridgeEthAddress{}; },
        [](P& p) { p.vault.token_address = Consensus::BridgeEthAddress{}; },
        [](P& p) { p.vault.token_address = p.vault.vault_address; },
        [](P& p) { p.vault.origin_decimals = 18; },
        [](P& p) { p.vault.asset_decimals = 8; },
        [](P& p) { p.vault.vault_runtime_code_hash = uint256{}; },
        [](P& p) { p.vault.token_runtime_code_hash = uint256{}; },
        [](P& p) { p.vault.proof_kind = 2; },
        [](P& p) { p.vault.proof_kind = 0; },
        [](P& p) { p.vault.origin_deployment_block = 0; },
        [](P& p) { p.vault.max_deposit_raw = 0; },
        [](P& p) { p.vault.max_deposit_raw = -1; },
        [](P& p) { p.vault.max_deposit_raw = p.vault.mint_caps.max_per_block + 1; },
        [](P& p) { p.vault.mint_caps.max_per_epoch = p.vault.mint_caps.max_per_block - 1; },
        [](P& p) { p.vault.mint_caps.epoch_length_blocks = 0; },
        [](P& p) { p.vault.mint_caps.max_per_block = 0; p.vault.max_deposit_raw = 0; },
        [](P& p) { p.vault.mint_caps.max_per_epoch = MAX_MONEY + 1; },
        // verifier
        [](P& p) { p.verifier.address = Consensus::BridgeEthAddress{}; },
        [](P& p) { p.verifier.address = p.vault.vault_address; },
        [](P& p) { p.verifier.address = p.vault.token_address; },
        [](P& p) { p.verifier.runtime_code_hash = uint256{}; },
        [](P& p) { p.verifier.min_bridge_validators = Consensus::MIN_BRIDGE_VALIDATORS_V1 - 1; },
        [](P& p) { p.verifier.max_bridge_validators = p.verifier.min_bridge_validators - 1; },
        [](P& p) { p.verifier.max_bridge_validators = Consensus::MAX_PROVEN_BRIDGE_VALIDATORS_V1 + 1; },
        [](P& p) { p.verifier.min_bridge_total_weight = 0; },
        [](P& p) { p.verifier.max_epoch_lag_seconds = 0; },
        // A verifier window other than the gate's set expiry: shorter would
        // keep burns open after the verifier stops accepting roots.
        [](P& p) { p.verifier.max_epoch_lag_seconds = p.gate.set_expiry_seconds - 1; },
        [](P& p) { p.verifier.max_epoch_lag_seconds = p.gate.set_expiry_seconds + 1; },
        // gate (every BridgeV2GateParams::Valid() refusal is in
        // bridge_v2_gate_params_profiles_and_refusals)
        [](P& p) { p.gate.observation_max_age_seconds = 0; },
        [](P& p) { p.gate.set_expiry_seconds = p.gate.release_margin_seconds + p.gate.burn_margin_seconds; },
    };
    for (size_t i{0}; i < refused.size(); ++i) {
        P p{RegtestV2Params()};
        refused[i](p);
        BOOST_CHECK_MESSAGE(!p.Valid(), "refusal " << i << " accepted");
    }

    // Origin chain 1 is exactly USDT under the mainnet beacon root with every
    // code-hash pin and the MAX_EPOCH_LAG mirror; a fixture copied with chain
    // id 1 fails closed.
    const std::vector<std::function<void(P&)>> mainnet_refused{
        [](P& p) { p.vault.token_address = FillAddress(0x55); },
        [](P& p) { p.light_client.genesis_validators_root = Fill(0x09); },
        [](P& p) { p.vault.vault_runtime_code_hash.reset(); },
        [](P& p) { p.verifier.runtime_code_hash.reset(); },
        [](P& p) { p.verifier.max_epoch_lag_seconds.reset(); },
    };
    for (size_t i{0}; i < mainnet_refused.size(); ++i) {
        P p{MainnetShapedV2Params()};
        mainnet_refused[i](p);
        BOOST_CHECK_MESSAGE(!p.Valid(), "mainnet refusal " << i << " accepted");
    }
    P copied{RegtestV2Params()};
    copied.vault.origin_chain_id = Consensus::BUSD_ETHEREUM_CHAIN_ID;
    BOOST_CHECK(!copied.Valid());
}

BOOST_AUTO_TEST_CASE(bridge_v2_gate_params_profiles_and_refusals)
{
    using G = Consensus::BridgeV2GateParams;
    // Profile M (B-13): the defaults, so a default envelope carries them.
    const G m{};
    BOOST_CHECK(m.Valid());
    BOOST_CHECK_EQUAL(m.set_expiry_seconds, 2'592'000);
    BOOST_CHECK_EQUAL(m.release_margin_seconds, 604'800);
    BOOST_CHECK_EQUAL(m.burn_margin_seconds, 86'400);
    BOOST_CHECK_EQUAL(m.observation_max_age_seconds, 172'800);
    BOOST_CHECK_EQUAL(m.max_epoch_gap, 2U);
    BOOST_CHECK_EQUAL(m.max_finality_lag_blocks, 90);
    BOOST_CHECK_EQUAL(m.wind_down_margin_ppm, 100'000U);
    BOOST_CHECK_EQUAL(m.mempool_close_margin_seconds, 3'600);
    // E2: 30 d - 7 d - 1 d (node changes section 8.4).
    BOOST_CHECK_EQUAL(m.RotationAgeLimit(), 1'900'800);
    BOOST_CHECK(Consensus::BridgeV2Params{}.gate == m);

    // Profile R (regtest plan section 3.3): times / 36, lag 30.
    const G r{G::RegtestProfileR()};
    BOOST_CHECK(r.Valid());
    BOOST_CHECK_EQUAL(r.set_expiry_seconds, 72'000);
    BOOST_CHECK_EQUAL(r.release_margin_seconds, 16'800);
    BOOST_CHECK_EQUAL(r.burn_margin_seconds, 2'400);
    BOOST_CHECK_EQUAL(r.observation_max_age_seconds, 4'800);
    BOOST_CHECK_EQUAL(r.mempool_close_margin_seconds, 100);
    BOOST_CHECK_EQUAL(r.max_finality_lag_blocks, 30);
    BOOST_CHECK_EQUAL(r.max_epoch_gap, m.max_epoch_gap);
    BOOST_CHECK_EQUAL(r.wind_down_margin_ppm, m.wind_down_margin_ppm);
    BOOST_CHECK_EQUAL(r.RotationAgeLimit(), 52'800);
    BOOST_CHECK_EQUAL(r.RotationAgeLimit() * 36, m.RotationAgeLimit());
    BOOST_CHECK(!(r == m));

    const std::vector<std::function<void(G&)>> accepted{
        [](G& g) { g.wind_down_margin_ppm = 0; },
        [](G& g) { g.wind_down_margin_ppm = 1'000'000; },
        [](G& g) { g.max_epoch_gap = 0; },
        [](G& g) { g.max_finality_lag_blocks = 1; },
        [](G& g) { g.set_expiry_seconds = g.release_margin_seconds + g.burn_margin_seconds + 1; },
        [](G& g) { g.set_expiry_seconds = 3; g.release_margin_seconds = 1; g.burn_margin_seconds = 1; },
    };
    for (size_t i{0}; i < accepted.size(); ++i) {
        G g{};
        accepted[i](g);
        BOOST_CHECK_MESSAGE(g.Valid(), "acceptance " << i << " refused");
        BOOST_CHECK_GT(g.RotationAgeLimit(), 0);
    }
    const std::vector<std::function<void(G&)>> refused{
        [](G& g) { g.set_expiry_seconds = 0; },
        [](G& g) { g.set_expiry_seconds = -1; },
        [](G& g) { g.release_margin_seconds = 0; },
        [](G& g) { g.release_margin_seconds = -1; },
        [](G& g) { g.burn_margin_seconds = 0; },
        [](G& g) { g.burn_margin_seconds = -1; },
        [](G& g) { g.observation_max_age_seconds = 0; },
        [](G& g) { g.observation_max_age_seconds = -1; },
        [](G& g) { g.mempool_close_margin_seconds = 0; },
        [](G& g) { g.mempool_close_margin_seconds = -1; },
        [](G& g) { g.wind_down_margin_ppm = 1'000'001; },
        [](G& g) { g.max_finality_lag_blocks = 0; },
        [](G& g) { g.max_finality_lag_blocks = -1; },
        // E2 must be positive: set_expiry > release_margin + burn_margin.
        [](G& g) { g.set_expiry_seconds = g.release_margin_seconds + g.burn_margin_seconds; },
        [](G& g) { g.release_margin_seconds = g.set_expiry_seconds; },
        [](G& g) { g.burn_margin_seconds = g.set_expiry_seconds; },
        // Huge margins cannot overflow the comparison.
        [](G& g) {
            g.release_margin_seconds = std::numeric_limits<int64_t>::max();
            g.burn_margin_seconds = std::numeric_limits<int64_t>::max();
        },
        [](G& g) {
            g.set_expiry_seconds = std::numeric_limits<int64_t>::max();
            g.release_margin_seconds = std::numeric_limits<int64_t>::max() - 1;
            g.burn_margin_seconds = 2;
        },
    };
    for (size_t i{0}; i < refused.size(); ++i) {
        G g{};
        refused[i](g);
        BOOST_CHECK_MESSAGE(!g.Valid(), "refusal " << i << " accepted");
        Consensus::BridgeV2Params p{RegtestV2Params()};
        p.gate = g;
        BOOST_CHECK_MESSAGE(!p.Valid(), "envelope refusal " << i << " accepted");
    }
}

BOOST_AUTO_TEST_CASE(bridge_v2_activation_predicates)
{
    Consensus::BridgeV2Params v2{RegtestV2Params()};
    v2.activation_height = 400;
    v2.burn_height = 405;
    const Consensus::Params params{ConsensusWith(v2)};
    BOOST_CHECK(!params.BridgeV2RulesActive(399));
    BOOST_CHECK(params.BridgeV2RulesActive(400));
    BOOST_CHECK(params.BridgeV2RulesActive(std::numeric_limits<int>::max()));
    BOOST_CHECK(!params.BridgeV2BurnRulesActive(400));
    BOOST_CHECK(!params.BridgeV2BurnRulesActive(404));
    BOOST_CHECK(params.BridgeV2BurnRulesActive(405));

    BOOST_CHECK(params.BridgeV2ConfigurationValid());

    // An incomplete envelope never activates, at any height, and fails the
    // startup assertion (init.cpp refuses to start, node changes 3.4).
    Consensus::BridgeV2Params incomplete{v2};
    incomplete.vault.token_runtime_code_hash = uint256{};
    const Consensus::Params bad{ConsensusWith(incomplete)};
    BOOST_CHECK(!bad.BridgeV2ConfigurationValid());
    BOOST_CHECK(!bad.BridgeV2RulesActive(std::numeric_limits<int>::max()));
    BOOST_CHECK(!bad.BridgeV2BurnRulesActive(std::numeric_limits<int>::max()));
    BOOST_CHECK(!modern::ConfiguredBridgeV2AssetId(bad));
    BOOST_CHECK(!modern::ConfiguredBridgeV2EntryId(bad));
    BOOST_CHECK(!modern::ConfiguredBridgeV2RegistryId(bad));

    const Consensus::Params unset{ConsensusWith(std::nullopt)};
    BOOST_CHECK(unset.BridgeV2ConfigurationValid());
    BOOST_CHECK(!unset.BridgeV2RulesActive(std::numeric_limits<int>::max()));
    BOOST_CHECK(!unset.BridgeV2BurnRulesActive(std::numeric_limits<int>::max()));
}

BOOST_AUTO_TEST_CASE(bridge_v2_entry_and_registry_ids)
{
    const Consensus::BridgeV2Params base{RegtestV2Params()};
    const uint256 domain{Fill(0x5d)};
    const uint256 entry_id{modern::BridgeVaultEntryIdV2(domain, base.vault)};
    const auto registry_id{modern::BridgeRegistryIdV2(domain, base)};
    BOOST_REQUIRE(registry_id);
    const modern::AssetId asset{modern::BridgeAssetIdV2(domain, base.vault)};
    // Distinct tags: the three identities never coincide.
    BOOST_CHECK(entry_id != asset);
    BOOST_CHECK(*registry_id != entry_id && *registry_id != asset);
    // Deterministic and domain-bound.
    BOOST_CHECK(modern::BridgeVaultEntryIdV2(domain, base.vault) == entry_id);
    BOOST_CHECK(modern::BridgeRegistryIdV2(domain, base) == registry_id);
    BOOST_CHECK(modern::BridgeVaultEntryIdV2(Fill(0x5e), base.vault) != entry_id);
    BOOST_CHECK(modern::BridgeRegistryIdV2(Fill(0x5e), base) != registry_id);

    // The id ends with the hash of the (empty) light-client extension list
    // (node changes section 3.2), independently re-derived with Python
    // hashlib: sha256(tag || tag || LE32(0)), tag = sha256(the tag string).
    BOOST_CHECK_EQUAL(EvmHex(modern::BridgeLightClientExtensionsHashV2()),
                      "e3c6c50c21ef35c5aeb887ab8f9794689b6d5f06cb9f777351d7bb169d385cba");

    // V and W2 are not part of either id.
    Consensus::BridgeV2Params moved{base};
    moved.activation_height = 900;
    moved.burn_height = 950;
    BOOST_CHECK(modern::BridgeRegistryIdV2(domain, moved) == registry_id);

    // The entry id commits the vault code hash (and whether it is pinned),
    // proof kind and the vault tuple, but not registry-only pins.
    Consensus::BridgeV2VaultEntry hashed{base.vault};
    hashed.vault_runtime_code_hash = Fill(0x0b);
    BOOST_CHECK(modern::BridgeVaultEntryIdV2(domain, hashed) != entry_id);
    Consensus::BridgeV2VaultEntry other_hash{hashed};
    other_hash.vault_runtime_code_hash = Fill(0x0d);
    BOOST_CHECK(modern::BridgeVaultEntryIdV2(domain, other_hash) !=
                modern::BridgeVaultEntryIdV2(domain, hashed));
    Consensus::BridgeV2VaultEntry kind{base.vault};
    kind.proof_kind = 2;
    BOOST_CHECK(modern::BridgeVaultEntryIdV2(domain, kind) != entry_id);
    Consensus::BridgeV2VaultEntry chain{base.vault};
    chain.origin_chain_id = 31'338;
    BOOST_CHECK(modern::BridgeVaultEntryIdV2(domain, chain) != entry_id);
    Consensus::BridgeV2VaultEntry registry_only{base.vault};
    registry_only.origin_deployment_block = 77;
    registry_only.token_runtime_code_hash = Fill(0x0e);
    registry_only.max_deposit_raw = 5;
    registry_only.mint_caps.epoch_length_blocks = 40;
    BOOST_CHECK(modern::BridgeVaultEntryIdV2(domain, registry_only) == entry_id);

    // Every sampled pin category renames the registry approval.
    using P = Consensus::BridgeV2Params;
    const std::vector<std::function<void(P&)>> renames{
        [](P& p) { p.vault.vault_runtime_code_hash = Fill(0x0b); },
        [](P& p) { p.vault.origin_deployment_block = 4; },
        [](P& p) { p.vault.token_runtime_code_hash = Fill(0x0e); },
        [](P& p) { p.vault.max_deposit_raw = 1; },
        [](P& p) { p.vault.mint_caps.max_per_epoch += 1; },
        [](P& p) { p.vault.mint_caps.epoch_length_blocks = 40; },
        [](P& p) { p.light_client.trusted_checkpoint_slot = 8'224; },
        [](P& p) { p.light_client.fork_schedule_valid_through_epoch = 2'048; },
        [](P& p) { p.light_client.max_sync_lag_slots = 1; },
        [](P& p) { p.verifier.address = FillAddress(0x67); },
        [](P& p) { p.verifier.runtime_code_hash = Fill(0x0c); },
        [](P& p) { p.verifier.min_bridge_validators = 5; },
        [](P& p) { p.verifier.max_bridge_validators = 63; },
        [](P& p) { p.verifier.min_bridge_total_weight = 5; },
        // Every consensus burn-gate threshold (node changes section 8.6).
        [](P& p) { p.gate.set_expiry_seconds += 1; },
        [](P& p) { p.gate.release_margin_seconds += 1; },
        [](P& p) { p.gate.burn_margin_seconds += 1; },
        [](P& p) { p.gate.observation_max_age_seconds += 1; },
        [](P& p) { p.gate.max_epoch_gap += 1; },
        [](P& p) { p.gate.max_finality_lag_blocks += 1; },
        [](P& p) { p.gate.wind_down_margin_ppm += 1; },
        [](P& p) { p.gate = Consensus::BridgeV2GateParams::RegtestProfileR(); },
    };
    for (size_t i{0}; i < renames.size(); ++i) {
        P p{base};
        renames[i](p);
        BOOST_REQUIRE(p.Valid());
        BOOST_CHECK_MESSAGE(modern::BridgeRegistryIdV2(domain, p) != registry_id,
                            "pin " << i << " does not rename the registry");
    }
    // Field order matters: equal sums in different fields still rename.
    P shifted{base};
    shifted.gate.release_margin_seconds += 1;
    shifted.gate.burn_margin_seconds -= 1;
    BOOST_REQUIRE(shifted.Valid());
    BOOST_CHECK(modern::BridgeRegistryIdV2(domain, shifted) != registry_id);
    // The mempool close margin is policy only and never renames.
    P policy{base};
    policy.gate.mempool_close_margin_seconds = 1;
    BOOST_REQUIRE(policy.Valid());
    BOOST_CHECK(modern::BridgeRegistryIdV2(domain, policy) == registry_id);

    // An invalid envelope has no registry identity.
    P invalid{base};
    invalid.vault.origin_deployment_block = 0;
    BOOST_CHECK(!modern::BridgeRegistryIdV2(domain, invalid));

    // Configured ids use the chain's modern domain.
    const Consensus::Params params{ConsensusWith(base)};
    const auto chain_domain{modern::ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash)};
    BOOST_REQUIRE(chain_domain);
    BOOST_CHECK(modern::ConfiguredBridgeV2AssetId(params) == modern::BridgeAssetIdV2(*chain_domain, base.vault));
    BOOST_CHECK(modern::ConfiguredBridgeV2EntryId(params) == modern::BridgeVaultEntryIdV2(*chain_domain, base.vault));
    BOOST_CHECK(modern::ConfiguredBridgeV2RegistryId(params) == modern::BridgeRegistryIdV2(*chain_domain, base));
    Consensus::Params no_final{params};
    no_final.legacy_final_hash.reset();
    BOOST_CHECK(!modern::ConfiguredBridgeV2AssetId(no_final));
    BOOST_CHECK(!modern::ConfiguredBridgeV2EntryId(no_final));
    BOOST_CHECK(!modern::ConfiguredBridgeV2RegistryId(no_final));
}

BOOST_AUTO_TEST_CASE(non_regtest_networks_have_no_bridge_v2)
{
    std::vector<std::unique_ptr<const CChainParams>> chains;
    chains.push_back(CChainParams::Main());
    chains.push_back(CChainParams::TestNet());
    chains.push_back(CChainParams::TestNet4());
    chains.push_back(CChainParams::SigNet(CChainParams::SigNetOptions{}));
    chains.push_back(CChainParams::RegTest(CChainParams::RegTestOptions{}));
    // The FlowMesh regtest profile without a pin file has no V2 envelope either.
    CChainParams::RegTestOptions flowmesh;
    CChainParams::B3ModernRegTestOptions modern_options;
    modern_options.flowmesh_test = true;
    flowmesh.b3_modern = modern_options;
    chains.push_back(CChainParams::RegTest(flowmesh));

    for (const auto& chain : chains) {
        const Consensus::Params& params{chain->GetConsensus()};
        BOOST_TEST_CONTEXT(chain->GetChainTypeString())
        {
            BOOST_CHECK(!params.bridge_v2);
            // init.cpp's startup assertion holds on every shipped network.
            BOOST_CHECK(params.BridgeV2ConfigurationValid());
            BOOST_CHECK(!params.BridgeV2RulesActive(std::numeric_limits<int>::max()));
            BOOST_CHECK(!params.BridgeV2BurnRulesActive(std::numeric_limits<int>::max()));
            BOOST_CHECK(!modern::ConfiguredBridgeV2AssetId(params));
            BOOST_CHECK(!modern::ConfiguredBridgeV2EntryId(params));
            BOOST_CHECK(!modern::ConfiguredBridgeV2RegistryId(params));
        }
    }
}

// ------------------------- forwarder pins (node changes 3.1; routes b, d3)

namespace {

//! Stand-in F_d / F_x pins: factories 0x71..71 and 0x73..73, init-code
//! hashes 0x72..72 and 0x74..74, dst_eid 30110 (Arbitrum).
Consensus::BridgeV2DepositForwarderPins DepositForwarderPins()
{
    return {.factory = FillAddress(0x71), .init_code_hash = Fill(0x72)};
}

Consensus::BridgeV2ExitForwarderPins ExitForwarderPins()
{
    return {.factory = FillAddress(0x73), .init_code_hash = Fill(0x74), .dst_eid = 30'110};
}

} // namespace

BOOST_AUTO_TEST_CASE(bridge_v2_forwarder_pins_valid_refusals)
{
    using P = Consensus::BridgeV2Params;
    const uint256 domain{FixtureDomain()};
    BOOST_CHECK(DepositForwarderPins().Valid());
    BOOST_CHECK(ExitForwarderPins().Valid());
    BOOST_CHECK(!Consensus::BridgeV2DepositForwarderPins{}.Valid());
    BOOST_CHECK(!Consensus::BridgeV2ExitForwarderPins{}.Valid());

    // Unset on a plain envelope, where ForwardersValid() is vacuous.
    const P base{RegtestV2Params()};
    BOOST_CHECK(!base.deposit_forwarder && !base.exit_forwarder);
    BOOST_CHECK(base.ForwardersValid());

    // Either pin, both, and both beside a successor keep the envelope valid;
    // the two factories may share one init-code hash (one B3Forwarder).
    const std::vector<std::function<void(P&)>> accepted{
        [](P& p) { p.deposit_forwarder = DepositForwarderPins(); },
        [](P& p) { p.exit_forwarder = ExitForwarderPins(); },
        [](P& p) { p.deposit_forwarder = DepositForwarderPins(); p.exit_forwarder = ExitForwarderPins(); },
        [](P& p) {
            p.deposit_forwarder = DepositForwarderPins();
            p.exit_forwarder = ExitForwarderPins();
            p.exit_forwarder->init_code_hash = p.deposit_forwarder->init_code_hash;
        },
        [](P& p) { p.exit_forwarder = ExitForwarderPins(); p.exit_forwarder->dst_eid = 30'101; },
        [](P& p) { p.exit_forwarder = ExitForwarderPins(); p.exit_forwarder->dst_eid = std::numeric_limits<uint32_t>::max(); },
    };
    for (size_t i{0}; i < accepted.size(); ++i) {
        P p{base};
        accepted[i](p);
        BOOST_CHECK_MESSAGE(p.ForwardersValid() && p.Valid(), "acceptance " << i << " refused");
        P with_successor{WithSuccessor(base, domain)};
        accepted[i](with_successor);
        BOOST_CHECK_MESSAGE(with_successor.Valid(), "acceptance " << i << " refused beside a successor");
    }

    // One mutation per ForwardersValid() rule.
    const std::vector<std::function<void(P&)>> refused{
        // incomplete pins
        [](P& p) { p.deposit_forwarder = DepositForwarderPins(); p.deposit_forwarder->factory = {}; },
        [](P& p) { p.deposit_forwarder = DepositForwarderPins(); p.deposit_forwarder->init_code_hash = uint256{}; },
        [](P& p) { p.exit_forwarder = ExitForwarderPins(); p.exit_forwarder->factory = {}; },
        [](P& p) { p.exit_forwarder = ExitForwarderPins(); p.exit_forwarder->init_code_hash = uint256{}; },
        [](P& p) { p.exit_forwarder = ExitForwarderPins(); p.exit_forwarder->dst_eid = 0; },
        // a factory on a bridge V2 contract
        [](P& p) { p.deposit_forwarder = DepositForwarderPins(); p.deposit_forwarder->factory = p.vault.vault_address; },
        [](P& p) { p.deposit_forwarder = DepositForwarderPins(); p.deposit_forwarder->factory = p.vault.token_address; },
        [](P& p) { p.deposit_forwarder = DepositForwarderPins(); p.deposit_forwarder->factory = p.verifier.address; },
        [](P& p) { p.exit_forwarder = ExitForwarderPins(); p.exit_forwarder->factory = p.vault.vault_address; },
        [](P& p) { p.exit_forwarder = ExitForwarderPins(); p.exit_forwarder->factory = p.vault.token_address; },
        [](P& p) { p.exit_forwarder = ExitForwarderPins(); p.exit_forwarder->factory = p.verifier.address; },
        // one factory for both routes
        [](P& p) {
            p.deposit_forwarder = DepositForwarderPins();
            p.exit_forwarder = ExitForwarderPins();
            p.exit_forwarder->factory = p.deposit_forwarder->factory;
        },
    };
    for (size_t i{0}; i < refused.size(); ++i) {
        P p{base};
        refused[i](p);
        BOOST_CHECK_MESSAGE(!p.ForwardersValid(), "refusal " << i << " accepted");
        BOOST_CHECK_MESSAGE(!p.Valid(), "envelope refusal " << i << " accepted");
        BOOST_CHECK_MESSAGE(!modern::BridgeRegistryIdV2(domain, p), "refusal " << i << " has a registry id");
        // Dropping the pins restores the launch verdict.
        p.deposit_forwarder.reset();
        p.exit_forwarder.reset();
        BOOST_CHECK(p.Valid());
    }

    // Neither factory may sit on a pinned successor vault.
    const P with_successor{WithSuccessor(base, domain)};
    BOOST_REQUIRE(with_successor.Valid());
    {
        P p{with_successor};
        p.deposit_forwarder = DepositForwarderPins();
        p.deposit_forwarder->factory = p.successor->entry.vault_address;
        BOOST_CHECK(!p.ForwardersValid());
        BOOST_CHECK(!p.Valid());
        P q{with_successor};
        q.exit_forwarder = ExitForwarderPins();
        q.exit_forwarder->factory = q.successor->entry.vault_address;
        BOOST_CHECK(!q.ForwardersValid());
        BOOST_CHECK(!q.Valid());
        // The same address is fine once no successor is pinned.
        q.successor.reset();
        BOOST_CHECK(q.Valid());
    }

    // Valid forwarder pins never repair an invalid launch envelope.
    P bad_launch{base};
    bad_launch.vault.origin_deployment_block = 0;
    bad_launch.deposit_forwarder = DepositForwarderPins();
    bad_launch.exit_forwarder = ExitForwarderPins();
    BOOST_CHECK(bad_launch.ForwardersValid());
    BOOST_CHECK(!bad_launch.Valid());
}

BOOST_AUTO_TEST_CASE(bridge_v2_forwarder_pins_registry_id)
{
    using P = Consensus::BridgeV2Params;
    const uint256 domain{Fill(0x5d)};
    const P base{RegtestV2Params()};
    P deposit_only{base};
    deposit_only.deposit_forwarder = DepositForwarderPins();
    P exit_only{base};
    exit_only.exit_forwarder = ExitForwarderPins();
    P both{deposit_only};
    both.exit_forwarder = ExitForwarderPins();

    // Golden values, independently re-derived with Python hashlib from the
    // Bitcoin serialization of BridgeRegistryIdV2 (LE integers, raw bytes):
    // the forwarder-free id predates the forwarder pins, so a forwarder-free
    // envelope keeps its id byte for byte; the tails are u8 2 || factory ||
    // init_code_hash and u8 3 || factory || init_code_hash || LE32 dst_eid,
    // after the gate thresholds and before the extension list's hash.
    const auto none_id{modern::BridgeRegistryIdV2(domain, base)};
    const auto deposit_id{modern::BridgeRegistryIdV2(domain, deposit_only)};
    const auto exit_id{modern::BridgeRegistryIdV2(domain, exit_only)};
    const auto both_id{modern::BridgeRegistryIdV2(domain, both)};
    BOOST_REQUIRE(none_id && deposit_id && exit_id && both_id);
    BOOST_CHECK_EQUAL(EvmHex(*none_id), "06c67bdb1cfaf39a00d4e46f432aaba74ff037cd20f3f0ad0dc4e33985bbee9b");
    BOOST_CHECK_EQUAL(EvmHex(*deposit_id), "2fa3cd957c53f836e46f55ab76f0a48429eed2653b0b6fd35a3c7d45cf7896f8");
    BOOST_CHECK_EQUAL(EvmHex(*exit_id), "7cf0eb62402a02203d25d60ebb6682fdaef6546d0562508fba86dcb6a230a365");
    BOOST_CHECK_EQUAL(EvmHex(*both_id), "81ff7faf9d0c857ac1085ac9e4fb63477a48d943b1e1f529f5905816922aa0d4");

    // Each pin, and each of its fields, renames the registry approval.
    const std::vector<std::pair<P, std::function<void(P&)>>> renames{
        {deposit_only, [](P& p) { p.deposit_forwarder->factory = FillAddress(0x75); }},
        {deposit_only, [](P& p) { p.deposit_forwarder->init_code_hash = Fill(0x76); }},
        {exit_only, [](P& p) { p.exit_forwarder->factory = FillAddress(0x75); }},
        {exit_only, [](P& p) { p.exit_forwarder->init_code_hash = Fill(0x76); }},
        {exit_only, [](P& p) { p.exit_forwarder->dst_eid = 30'101; }},
        {both, [](P& p) { p.deposit_forwarder->factory = FillAddress(0x75); }},
        {both, [](P& p) { p.exit_forwarder->dst_eid = 30'111; }},
    };
    for (size_t i{0}; i < renames.size(); ++i) {
        const auto& [start, mutate] = renames[i];
        const auto before{modern::BridgeRegistryIdV2(domain, start)};
        P p{start};
        mutate(p);
        BOOST_REQUIRE(p.Valid());
        const auto after{modern::BridgeRegistryIdV2(domain, p)};
        BOOST_REQUIRE(before && after);
        BOOST_CHECK_MESSAGE(*after != *before, "forwarder field " << i << " does not rename the registry");
        BOOST_CHECK_MESSAGE(*after != *none_id, "forwarder field " << i << " collides with the forwarder-free id");
    }

    // The role byte separates the routes: the same factory and hash pinned as
    // a deposit or as an exit forwarder give different ids.
    P deposit_like{base};
    deposit_like.deposit_forwarder = Consensus::BridgeV2DepositForwarderPins{
        .factory = ExitForwarderPins().factory, .init_code_hash = ExitForwarderPins().init_code_hash};
    BOOST_REQUIRE(deposit_like.Valid());
    BOOST_CHECK(*modern::BridgeRegistryIdV2(domain, deposit_like) != *exit_id);

    // Forwarder pins are derivation and burn-check pins only: the AssetId and
    // the vault entry id that records name are unchanged.
    BOOST_CHECK(modern::BridgeAssetIdV2(domain, both.vault) == modern::BridgeAssetIdV2(domain, base.vault));
    BOOST_CHECK(modern::BridgeVaultEntryIdV2(domain, both.vault) == modern::BridgeVaultEntryIdV2(domain, base.vault));

    // Beside a successor the forwarder tail follows the successor's block;
    // the successor-only id stays as it was.
    const P successor_base{WithSuccessor(base, domain)};
    P successor_both{successor_base};
    successor_both.deposit_forwarder = DepositForwarderPins();
    successor_both.exit_forwarder = ExitForwarderPins();
    const auto successor_id{modern::BridgeRegistryIdV2(domain, successor_base)};
    const auto successor_both_id{modern::BridgeRegistryIdV2(domain, successor_both)};
    BOOST_REQUIRE(successor_id && successor_both_id);
    BOOST_CHECK(*successor_both_id != *successor_id);
    BOOST_CHECK(*successor_both_id != *both_id);

    // V, W2 and the policy margin still stay out with forwarders pinned.
    P moved{both};
    moved.activation_height = 900;
    moved.burn_height = 950;
    moved.gate.mempool_close_margin_seconds = 1;
    BOOST_CHECK(modern::BridgeRegistryIdV2(domain, moved) == both_id);

    // The configured id follows the chain's envelope.
    const Consensus::Params params{ConsensusWith(both)};
    const auto chain_domain{modern::ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash)};
    BOOST_REQUIRE(chain_domain);
    BOOST_CHECK(modern::ConfiguredBridgeV2RegistryId(params) == modern::BridgeRegistryIdV2(*chain_domain, both));
    BOOST_CHECK(*modern::ConfiguredBridgeV2RegistryId(params) != *modern::ConfiguredBridgeV2RegistryId(ConsensusWith(base)));
}

// The wallets' static and height refusals of the forwarder routes
// (bridgeforwarderaddress and bridgewithdraw options.arbitrum): the exact
// texts the RPCs throw with RPC_MISC_ERROR, on every network.
BOOST_AUTO_TEST_CASE(bridge_v2_forwarder_route_availability)
{
    using P = Consensus::BridgeV2Params;
    const auto deposit_reason{[](const Consensus::Params& params, const int height) {
        return std::string{modern::DepositForwarderUnavailableReasonV2(params, height).value_or("")};
    }};
    const auto exit_reason{[](const Consensus::Params& params, const int height) {
        return std::string{modern::ExitForwarderRouteUnavailableReasonV2(params, height).value_or("")};
    }};
    const std::string not_configured{"Bridge V2 is not configured on this chain"};
    const std::string no_deposit_pin{"Bridge V2 pins no deposit forwarder (route b) on this chain"};
    const std::string retired{
        "The bridge V2 launch vault is retired for deposits and the deposit forwarder factory deposits only "
        "into it; use getbridgedepositrecipient for the successor vault"};
    const std::string no_exit_pin{
        "options.arbitrum (route d3) needs a pinned bridge V2 exit forwarder; this chain pins none"};
    const std::string no_burns{"options.arbitrum (route d3) needs bridge V2 burn rules for the next block"};

    // No bridge V2, and an envelope without forwarder pins.
    for (const Consensus::Params& params : {ConsensusWith(std::nullopt), ConsensusWith(RegtestV2Params())}) {
        const bool configured{params.bridge_v2.has_value()};
        BOOST_CHECK_EQUAL(deposit_reason(params, 500), configured ? no_deposit_pin : not_configured);
        BOOST_CHECK_EQUAL(exit_reason(params, 500), no_exit_pin);
    }

    // Both pins, V = W2 = 400 (and W2 = 410).
    P both{RegtestV2Params()};
    both.deposit_forwarder = DepositForwarderPins();
    both.exit_forwarder = ExitForwarderPins();
    const Consensus::Params pinned{ConsensusWith(both)};
    // The deposit route has no height refusal of its own below V: the RPC
    // then refuses on B3's deposit status ("Bridge V2 rules are not active").
    BOOST_CHECK_EQUAL(deposit_reason(pinned, 399), "");
    BOOST_CHECK_EQUAL(deposit_reason(pinned, 400), "");
    BOOST_CHECK_EQUAL(exit_reason(pinned, 399), no_burns);
    BOOST_CHECK_EQUAL(exit_reason(pinned, 400), "");
    P late_burns{both};
    late_burns.burn_height = 410;
    BOOST_CHECK_EQUAL(exit_reason(ConsensusWith(late_burns), 409), no_burns);
    BOOST_CHECK_EQUAL(exit_reason(ConsensusWith(late_burns), 410), "");

    // Either pin alone.
    P deposit_only{both};
    deposit_only.exit_forwarder.reset();
    BOOST_CHECK_EQUAL(deposit_reason(ConsensusWith(deposit_only), 400), "");
    BOOST_CHECK_EQUAL(exit_reason(ConsensusWith(deposit_only), 400), no_exit_pin);
    P exit_only{both};
    exit_only.deposit_forwarder.reset();
    BOOST_CHECK_EQUAL(deposit_reason(ConsensusWith(exit_only), 400), no_deposit_pin);
    BOOST_CHECK_EQUAL(exit_reason(ConsensusWith(exit_only), 400), "");

    // A B-23 successor (h_m = V + 60): the launch vault is retired for the
    // deposit factory from h_m; route (d3) burns go on, naming the successor.
    const Consensus::Params migrating{ConsensusWith(WithSuccessor(both, FixtureDomain()))};
    BOOST_CHECK_EQUAL(deposit_reason(migrating, 459), "");
    BOOST_CHECK_EQUAL(deposit_reason(migrating, 460), retired);
    BOOST_CHECK_EQUAL(exit_reason(migrating, 460), "");

    // An incomplete envelope configures neither route.
    P broken{both};
    broken.verifier.address = {};
    BOOST_REQUIRE(!broken.Valid());
    BOOST_CHECK_EQUAL(deposit_reason(ConsensusWith(broken), 500), not_configured);
    BOOST_CHECK_EQUAL(exit_reason(ConsensusWith(broken), 500), no_burns);
}

// ------------------------------- B-23 successor entry (node changes 7.6)

BOOST_AUTO_TEST_CASE(bridge_v2_successor_params_valid_refusals)
{
    using P = Consensus::BridgeV2Params;
    using Consensus::BridgeV2EntryRole;
    const uint256 domain{FixtureDomain()};
    const P base{WithSuccessor(RegtestV2Params(), domain)};
    BOOST_REQUIRE(base.successor);
    BOOST_CHECK(base.Valid());
    BOOST_CHECK(base.SuccessorValid());

    // Entry(): the launch entry always, the successor only when pinned.
    BOOST_CHECK(base.Entry(BridgeV2EntryRole::LAUNCH) == &base.vault);
    BOOST_CHECK(base.Entry(BridgeV2EntryRole::SUCCESSOR) == &base.successor->entry);
    const P launch_only{RegtestV2Params()};
    BOOST_CHECK(launch_only.Entry(BridgeV2EntryRole::LAUNCH) == &launch_only.vault);
    BOOST_CHECK(launch_only.Entry(BridgeV2EntryRole::SUCCESSOR) == nullptr);
    BOOST_CHECK(launch_only.SuccessorValid());

    // Without a successor Valid() is the launch envelope's verdict, valid or
    // not; a successor never repairs an invalid launch envelope.
    P removed{base};
    removed.successor.reset();
    BOOST_CHECK(removed.Valid());
    P bad_launch{base};
    bad_launch.vault.origin_deployment_block = 0;
    BOOST_CHECK(bad_launch.SuccessorValid());
    BOOST_CHECK(!bad_launch.Valid());
    bad_launch.successor.reset();
    BOOST_CHECK(!bad_launch.Valid());

    // Mutations that keep the successor valid.
    const std::vector<std::function<void(P&)>> accepted{
        // v' has its own MAX_DEPOSIT_RAW (at most the shared block cap) and
        // deployment block (a lower bound is safe).
        [](P& p) { p.successor->entry.max_deposit_raw = 1; },
        [](P& p) { p.successor->entry.origin_deployment_block = 1; },
        [](P& p) { p.successor->entry.vault_runtime_code_hash = Fill(0x0f); },
        // Its code hash may equal v's (the same build at another address).
        [](P& p) { p.vault.vault_runtime_code_hash = Fill(0x0f); p.successor->entry.vault_runtime_code_hash = Fill(0x0f); },
        [](P& p) { p.successor->migration_height = p.activation_height + 1; },
        [](P& p) { p.burn_height = p.activation_height + 10; p.successor->migration_height = p.burn_height; },
        [](P& p) { p.successor->migration_height = std::numeric_limits<int32_t>::max(); },
    };
    for (size_t i{0}; i < accepted.size(); ++i) {
        P p{base};
        accepted[i](p);
        BOOST_CHECK_MESSAGE(p.Valid(), "acceptance " << i << " refused");
    }

    // One field broken at a time.
    const std::vector<std::function<void(P&)>> refused{
        // v's chain, token, decimals, token code hash and proof kind
        [](P& p) { p.successor->entry.origin_chain_id = 31'338; },
        [](P& p) { p.successor->entry.token_address = FillAddress(0x56); },
        [](P& p) { p.successor->entry.origin_decimals = 18; },
        [](P& p) { p.successor->entry.asset_decimals = 8; },
        [](P& p) { p.successor->entry.token_runtime_code_hash = Fill(0x0e); },
        [](P& p) { p.successor->entry.proof_kind = 2; },
        // v's caps, each alone (the epoch counters are keyed by the shared
        // AssetId); the first keeps MAX_DEPOSIT_RAW within the lower cap
        [](P& p) {
            p.successor->entry.mint_caps.max_per_block -= 1;
            p.successor->entry.max_deposit_raw = p.successor->entry.mint_caps.max_per_block;
        },
        [](P& p) { p.successor->entry.mint_caps.max_per_epoch += 1; },
        [](P& p) { p.successor->entry.mint_caps.epoch_length_blocks = 40; },
        // an incomplete entry
        [](P& p) { p.successor->entry.vault_address = Consensus::BridgeEthAddress{}; },
        [](P& p) { p.successor->entry.origin_deployment_block = 0; },
        [](P& p) { p.successor->entry.max_deposit_raw = 0; },
        [](P& p) { p.successor->entry.max_deposit_raw = p.successor->entry.mint_caps.max_per_block + 1; },
        [](P& p) { p.successor->entry.vault_runtime_code_hash = uint256{}; },
        // vault reuse against v, the token and the verifier
        [](P& p) { p.successor->entry.vault_address = p.vault.vault_address; },
        [](P& p) { p.successor->entry.vault_address = p.vault.token_address; },
        [](P& p) { p.successor->entry.vault_address = p.verifier.address; },
        // predecessor and AssetId
        [](P& p) { p.successor->predecessor = FillAddress(0x47); },
        [](P& p) { p.successor->predecessor = p.successor->entry.vault_address; },
        [](P& p) { p.successor->predecessor = Consensus::BridgeEthAddress{}; },
        [](P& p) { p.successor->asset_id = uint256{}; },
        // h_m > V and h_m >= W2
        [](P& p) { p.successor->migration_height = p.activation_height; },
        [](P& p) { p.successor->migration_height = p.activation_height - 1; },
        [](P& p) { p.successor->migration_height = -1; },
        [](P& p) { p.burn_height = p.activation_height + 10; p.successor->migration_height = p.burn_height - 1; },
    };
    for (size_t i{0}; i < refused.size(); ++i) {
        P p{base};
        refused[i](p);
        BOOST_CHECK_MESSAGE(!p.SuccessorValid(), "refusal " << i << " accepted by SuccessorValid");
        BOOST_CHECK_MESSAGE(!p.Valid(), "refusal " << i << " accepted");
        // Dropping the broken successor restores the launch verdict.
        p.successor.reset();
        BOOST_CHECK_MESSAGE(p.Valid(), "launch envelope of refusal " << i << " refused");
    }

    // On Ethereum mainnet v' needs its vault code hash (OI-3 leaves it
    // unpinned only off mainnet); the regtest base above has none.
    BOOST_CHECK(!base.successor->entry.vault_runtime_code_hash);
    P mainnet{WithSuccessor(MainnetShapedV2Params(), domain)};
    BOOST_CHECK(!mainnet.Valid());
    mainnet.successor->entry.vault_runtime_code_hash = Fill(0x0f);
    BOOST_CHECK(mainnet.Valid());
    P mainnet_wrong_token{mainnet};
    mainnet_wrong_token.successor->entry.token_address = FillAddress(0x55);
    BOOST_CHECK(!mainnet_wrong_token.Valid());
}

BOOST_AUTO_TEST_CASE(bridge_v2_successor_ids)
{
    using P = Consensus::BridgeV2Params;
    using Consensus::BridgeV2EntryRole;
    const uint256 domain{Fill(0x5d)};
    const P launch{RegtestV2Params()};
    const P mainnet{MainnetShapedV2Params()};

    // Without a successor every id is byte-identical to the values the code
    // produced before successors existed (captured from the previous build).
    BOOST_CHECK_EQUAL(EvmHex(*modern::BridgeRegistryIdV2(domain, launch)),
                      "06c67bdb1cfaf39a00d4e46f432aaba74ff037cd20f3f0ad0dc4e33985bbee9b");
    BOOST_CHECK_EQUAL(EvmHex(modern::BridgeVaultEntryIdV2(domain, launch.vault)),
                      "685a6ba3c00f7d4018cdc53b210f3e9d26525f4e35eef7c094d0b51358408233");
    BOOST_CHECK_EQUAL(EvmHex(modern::BridgeAssetIdV2(domain, launch.vault)),
                      "3b8151a292eb0041453b523e611e50dd6b0010a62469fc98e4dc9a6932c11162");
    BOOST_CHECK_EQUAL(EvmHex(*modern::BridgeRegistryIdV2(domain, mainnet)),
                      "94ad4570376efc6bb918815783e4b744cf0ea6f54acf76fc241d1a27b7a57e34");
    BOOST_CHECK_EQUAL(EvmHex(modern::BridgeVaultEntryIdV2(domain, mainnet.vault)),
                      "5e56ae9a9e91004a5429d9dc5f00ac0c0c787c3e04781f61a321ae61330c59aa");
    BOOST_CHECK_EQUAL(EvmHex(modern::BridgeAssetIdV2(domain, mainnet.vault)),
                      "667c1fef8ff5ab49642d296061bcb91cd3d5d070c99321b44efa76ffdba052bd");
    BOOST_CHECK_EQUAL(EvmHex(*modern::ConfiguredBridgeV2RegistryId(ConsensusWith(launch))),
                      "0a6c4f32f5d42dfd01e8588616b0b954d72f984c77fbc4b9be2ffd4c3cdf561c");
    BOOST_CHECK_EQUAL(EvmHex(*modern::ConfiguredBridgeV2RegistryId(ConsensusWith(mainnet))),
                      "0b2fe9037f269c7527bc15fc4455868d9a4fabd955fd2f2c4b947c894c847888");

    const P with{WithSuccessor(launch, domain)};
    BOOST_REQUIRE(with.Valid());
    const uint256 launch_id{modern::BridgeVaultEntryIdV2(domain, with.vault)};
    const uint256 next_id{modern::BridgeVaultEntryIdV2(domain, with.successor->entry)};
    BOOST_CHECK(next_id != launch_id);
    // v' carries v's AssetId, not the one its own address would derive.
    BOOST_CHECK(with.successor->asset_id == modern::BridgeAssetIdV2(domain, with.vault));
    BOOST_CHECK(modern::BridgeAssetIdV2(domain, with.successor->entry) != with.successor->asset_id);

    // A successor renames the registry approval; h_m (like V and W2) does not.
    const auto registry_launch{modern::BridgeRegistryIdV2(domain, launch)};
    const auto registry_with{modern::BridgeRegistryIdV2(domain, with)};
    BOOST_REQUIRE(registry_launch && registry_with);
    BOOST_CHECK(*registry_with != *registry_launch);
    P moved{with};
    moved.successor->migration_height += 100;
    BOOST_REQUIRE(moved.Valid());
    BOOST_CHECK(modern::BridgeRegistryIdV2(domain, moved) == registry_with);
    P moved_v{with};
    moved_v.activation_height += 10;
    moved_v.burn_height += 20;
    BOOST_REQUIRE(moved_v.Valid());
    BOOST_CHECK(modern::BridgeRegistryIdV2(domain, moved_v) == registry_with);

    // Every successor-only pin renames it.
    const std::vector<std::function<void(P&)>> renames{
        [](P& p) { p.successor->entry.vault_address = FillAddress(0x48); },
        [](P& p) { p.successor->entry.vault_runtime_code_hash = Fill(0x0f); },
        [](P& p) { p.successor->entry.origin_deployment_block += 1; },
        [](P& p) { p.successor->entry.max_deposit_raw = 1; },
    };
    for (size_t i{0}; i < renames.size(); ++i) {
        P p{with};
        renames[i](p);
        BOOST_REQUIRE(p.Valid());
        const auto renamed{modern::BridgeRegistryIdV2(domain, p)};
        BOOST_REQUIRE(renamed);
        BOOST_CHECK_MESSAGE(*renamed != *registry_with && *renamed != *registry_launch,
                            "successor pin " << i << " does not rename the registry");
    }

    // A successor carrying any AssetId other than v's under this domain has
    // no registry identity, even though Valid() cannot see the domain.
    P wrong_asset{with};
    wrong_asset.successor->asset_id = Fill(0x77);
    BOOST_REQUIRE(wrong_asset.Valid());
    BOOST_CHECK(!modern::BridgeRegistryIdV2(domain, wrong_asset));
    BOOST_CHECK(!modern::BridgeRegistryIdV2(Fill(0x5e), with));

    // Configured identities on the chain's own domain.
    const uint256 chain_domain{FixtureDomain()};
    const P configured{WithSuccessor(RegtestV2Params(), chain_domain)};
    const Consensus::Params params{ConsensusWith(configured)};
    const modern::AssetId asset{modern::BridgeAssetIdV2(chain_domain, configured.vault)};
    BOOST_CHECK(modern::ConfiguredBridgeV2AssetId(params) == asset);
    BOOST_CHECK(modern::ConfiguredBridgeV2EntryId(params) ==
                modern::BridgeVaultEntryIdV2(chain_domain, configured.vault));
    BOOST_CHECK(modern::ConfiguredBridgeV2EntryId(params, BridgeV2EntryRole::LAUNCH) ==
                modern::ConfiguredBridgeV2EntryId(params));
    BOOST_CHECK(modern::ConfiguredBridgeV2EntryId(params, BridgeV2EntryRole::SUCCESSOR) ==
                modern::BridgeVaultEntryIdV2(chain_domain, configured.successor->entry));
    BOOST_CHECK(modern::ConfiguredBridgeV2RegistryId(params) ==
                modern::BridgeRegistryIdV2(chain_domain, configured));
    const Consensus::Params launch_params{ConsensusWith(RegtestV2Params())};
    BOOST_CHECK(modern::ConfiguredBridgeV2EntryId(launch_params, BridgeV2EntryRole::LAUNCH));
    BOOST_CHECK(!modern::ConfiguredBridgeV2EntryId(launch_params, BridgeV2EntryRole::SUCCESSOR));
    // The launch ids do not change when a successor is pinned.
    BOOST_CHECK(modern::ConfiguredBridgeV2AssetId(launch_params) == asset);
    BOOST_CHECK(modern::ConfiguredBridgeV2EntryId(launch_params) == modern::ConfiguredBridgeV2EntryId(params));

    // A mismatched AssetId fails closed: every configured V2 identity is
    // unavailable (node changes section 3.4 assertion 6). Both plausible
    // mistakes: the id the successor's own address derives, and v's id on another domain.
    for (const uint256& wrong : {modern::BridgeAssetIdV2(chain_domain, configured.successor->entry),
                                 modern::BridgeAssetIdV2(domain, configured.vault)}) {
        P mismatched{configured};
        mismatched.successor->asset_id = wrong;
        BOOST_REQUIRE(mismatched.Valid());
        const Consensus::Params bad{ConsensusWith(mismatched)};
        BOOST_CHECK(bad.BridgeV2ConfigurationValid());
        BOOST_CHECK(!modern::ConfiguredBridgeV2AssetId(bad));
        BOOST_CHECK(!modern::ConfiguredBridgeV2EntryId(bad));
        BOOST_CHECK(!modern::ConfiguredBridgeV2EntryId(bad, BridgeV2EntryRole::LAUNCH));
        BOOST_CHECK(!modern::ConfiguredBridgeV2EntryId(bad, BridgeV2EntryRole::SUCCESSOR));
        BOOST_CHECK(!modern::ConfiguredBridgeV2RegistryId(bad));
    }
}

BOOST_AUTO_TEST_CASE(bridge_v2_migration_predicates)
{
    using P = Consensus::BridgeV2Params;
    using Consensus::BridgeV2EntryRole;
    constexpr int MAX_HEIGHT{std::numeric_limits<int>::max()};
    P v2{WithSuccessor(RegtestV2Params(), FixtureDomain())};
    v2.burn_height = v2.activation_height + 5;
    BOOST_REQUIRE(v2.Valid());
    const int v{v2.activation_height};
    const int h_m{v2.successor->migration_height};
    BOOST_REQUIRE_GT(h_m, v2.burn_height);
    const Consensus::Params params{ConsensusWith(v2)};

    BOOST_CHECK(!params.BridgeV2MigrationActive(v - 1));
    BOOST_CHECK(!params.BridgeV2MigrationActive(v));
    BOOST_CHECK(!params.BridgeV2MigrationActive(h_m - 1));
    BOOST_CHECK(params.BridgeV2RulesActive(h_m - 1));
    BOOST_CHECK(params.BridgeV2BurnRulesActive(h_m - 1));
    BOOST_CHECK(params.BridgeV2MigrationActive(h_m));
    BOOST_CHECK(params.BridgeV2MigrationActive(MAX_HEIGHT));

    BOOST_CHECK(modern::ActiveBridgeV2Role(params, v - 1) == BridgeV2EntryRole::LAUNCH);
    BOOST_CHECK(modern::ActiveBridgeV2Role(params, v) == BridgeV2EntryRole::LAUNCH);
    BOOST_CHECK(modern::ActiveBridgeV2Role(params, h_m - 1) == BridgeV2EntryRole::LAUNCH);
    BOOST_CHECK(modern::ActiveBridgeV2Role(params, h_m) == BridgeV2EntryRole::SUCCESSOR);
    BOOST_CHECK(modern::ActiveBridgeV2Role(params, MAX_HEIGHT) == BridgeV2EntryRole::SUCCESSOR);

    const auto launch_id{modern::ConfiguredBridgeV2EntryId(params, BridgeV2EntryRole::LAUNCH)};
    const auto next_id{modern::ConfiguredBridgeV2EntryId(params, BridgeV2EntryRole::SUCCESSOR)};
    BOOST_REQUIRE(launch_id && next_id);
    using R = std::optional<BridgeV2EntryRole>;
    // Below V nothing resolves; the successor never resolves below h_m.
    BOOST_CHECK(modern::ResolveBridgeV2Entry(params, *launch_id, v - 1) == R{});
    BOOST_CHECK(modern::ResolveBridgeV2Entry(params, *next_id, v - 1) == R{});
    BOOST_CHECK(modern::ResolveBridgeV2Entry(params, *launch_id, v) == R{BridgeV2EntryRole::LAUNCH});
    BOOST_CHECK(modern::ResolveBridgeV2Entry(params, *next_id, v) == R{});
    BOOST_CHECK(modern::ResolveBridgeV2Entry(params, *next_id, h_m - 1) == R{});
    BOOST_CHECK(modern::ResolveBridgeV2Entry(params, *next_id, h_m) == R{BridgeV2EntryRole::SUCCESSOR});
    BOOST_CHECK(modern::ResolveBridgeV2Entry(params, *next_id, MAX_HEIGHT) == R{BridgeV2EntryRole::SUCCESSOR});
    // The retired launch entry still resolves; each record kind decides
    // what it may do with it (mints and sweeps yes, burns no).
    BOOST_CHECK(modern::ResolveBridgeV2Entry(params, *launch_id, h_m) == R{BridgeV2EntryRole::LAUNCH});
    // No other identity is an entry.
    for (const uint256& other : {*modern::ConfiguredBridgeV2AssetId(params),
                                 *modern::ConfiguredBridgeV2RegistryId(params), uint256{}, Fill(0x46)}) {
        BOOST_CHECK(modern::ResolveBridgeV2Entry(params, other, h_m) == R{});
    }

    // Without a successor nothing migrates.
    const Consensus::Params launch_only{ConsensusWith(RegtestV2Params())};
    BOOST_CHECK(!launch_only.BridgeV2MigrationActive(MAX_HEIGHT));
    BOOST_CHECK(modern::ActiveBridgeV2Role(launch_only, MAX_HEIGHT) == BridgeV2EntryRole::LAUNCH);
    BOOST_CHECK(modern::ResolveBridgeV2Entry(launch_only, *launch_id, MAX_HEIGHT) == R{BridgeV2EntryRole::LAUNCH});
    BOOST_CHECK(modern::ResolveBridgeV2Entry(launch_only, *next_id, MAX_HEIGHT) == R{});

    // An invalid successor invalidates the envelope: nothing activates and
    // init.cpp's startup assertion refuses it.
    P invalid{v2};
    invalid.successor->migration_height = invalid.activation_height;
    const Consensus::Params bad{ConsensusWith(invalid)};
    BOOST_CHECK(!bad.BridgeV2ConfigurationValid());
    BOOST_CHECK(!bad.BridgeV2RulesActive(MAX_HEIGHT));
    BOOST_CHECK(!bad.BridgeV2MigrationActive(MAX_HEIGHT));
    BOOST_CHECK(modern::ResolveBridgeV2Entry(bad, *launch_id, MAX_HEIGHT) == R{});

    // A mismatched AssetId leaves no entry resolvable at any height.
    P mismatched{v2};
    mismatched.successor->asset_id = Fill(0x77);
    const Consensus::Params unresolvable{ConsensusWith(mismatched)};
    for (const int h : {v, h_m - 1, h_m, MAX_HEIGHT}) {
        BOOST_CHECK(modern::ResolveBridgeV2Entry(unresolvable, *launch_id, h) == R{});
        BOOST_CHECK(modern::ResolveBridgeV2Entry(unresolvable, *next_id, h) == R{});
    }

    // Main, test, testnet4 and signet never set bridge_v2: no migration, at
    // any height.
    std::vector<std::unique_ptr<const CChainParams>> chains;
    chains.push_back(CChainParams::Main());
    chains.push_back(CChainParams::TestNet());
    chains.push_back(CChainParams::TestNet4());
    chains.push_back(CChainParams::SigNet(CChainParams::SigNetOptions{}));
    for (const auto& chain : chains) {
        const Consensus::Params& shipped{chain->GetConsensus()};
        BOOST_TEST_CONTEXT(chain->GetChainTypeString())
        {
            BOOST_CHECK(!shipped.bridge_v2);
            for (const int h : {0, 1, v, h_m, 810'000, 820'001, MAX_HEIGHT}) {
                BOOST_CHECK(!shipped.BridgeV2MigrationActive(h));
                BOOST_CHECK(modern::ActiveBridgeV2Role(shipped, h) == BridgeV2EntryRole::LAUNCH);
                BOOST_CHECK(modern::ResolveBridgeV2Entry(shipped, *launch_id, h) == R{});
                BOOST_CHECK(modern::ResolveBridgeV2Entry(shipped, *next_id, h) == R{});
            }
            BOOST_CHECK(!modern::ConfiguredBridgeV2EntryId(shipped, BridgeV2EntryRole::SUCCESSOR));
        }
    }
}

BOOST_AUTO_TEST_CASE(migration_leaf_matches_shared_vector)
{
    // The MIGRATE leaf is an ordinary leaf V2 (contracts spec section 6.4):
    // the shared vector read as retired vault 0x22..22 paying successor
    // 0x33..33 reproduces leaf_v2[0] byte for byte.
    Consensus::BridgeV2VaultEntry retired;
    retired.origin_chain_id = 1;
    retired.vault_address = Address(STAND_IN_VAULT);
    retired.token_address = Address(USDT);
    const Consensus::BridgeEthAddress successor_vault{Address(STAND_IN_RECIPIENT)};
    const modern::BridgeWithdrawalV2 w{
        modern::MakeVaultSweepWithdrawalV2(retired, Fill(0x11), successor_vault, 7, 1'000'000, 900'000)};
    BOOST_CHECK(w == VectorWithdrawal(7));
    BOOST_CHECK(w.vault == retired.vault_address);
    BOOST_CHECK(w.token == retired.token_address);
    BOOST_CHECK(w.recipient == successor_vault);
    BOOST_CHECK_EQUAL(w.destination_chain_id, retired.origin_chain_id);

    const auto preimage{modern::EncodeBridgeWithdrawalV2(w)};
    BOOST_REQUIRE(preimage);
    BOOST_CHECK_EQUAL(preimage->size(), 180U);
    BOOST_CHECK_EQUAL(HexStr(*preimage), LEAF_V2_PREIMAGE);
    const auto leaf{modern::BridgeWithdrawalLeafV2(w)};
    BOOST_REQUIRE(leaf);
    BOOST_CHECK_EQUAL(EvmHex(*leaf), LEAF_V2);

    // Only v can release it: the same fields under v' are another leaf.
    auto at_successor{w};
    at_successor.vault = successor_vault;
    at_successor.recipient = retired.vault_address;
    BOOST_CHECK(modern::BridgeWithdrawalLeafV2(at_successor) != leaf);

    // It takes its place in the one global V2 tree like any other leaf.
    const auto prefix{VectorPrefix()};
    modern::WithdrawalTreeState tree;
    for (size_t i{0}; i < 7; ++i) BOOST_REQUIRE(modern::AppendBridgeWithdrawalV2(tree, prefix[i]));
    BOOST_CHECK(modern::AppendBridgeWithdrawalV2(tree, w) == leaf);
    BOOST_CHECK_EQUAL(EvmHex(tree.root), TREE_V2_ROOT);

    // From an envelope: v's entry, the shared AssetId, the successor's vault.
    const uint256 domain{FixtureDomain()};
    const Consensus::BridgeV2Params v2{WithSuccessor(RegtestV2Params(), domain)};
    const auto sweep{modern::MakeVaultSweepWithdrawalV2(v2.vault, v2.successor->asset_id,
                                                        v2.successor->entry.vault_address, 3, 5'000'000, 461)};
    BOOST_CHECK_EQUAL(sweep.withdrawal_id, 3U);
    BOOST_CHECK_EQUAL(sweep.destination_chain_id, 31'337U);
    BOOST_CHECK(sweep.asset_id == modern::BridgeAssetIdV2(domain, v2.vault));
    BOOST_CHECK(sweep.vault == v2.vault.vault_address);
    BOOST_CHECK(sweep.token == v2.vault.token_address);
    BOOST_CHECK(sweep.recipient == FillAddress(0x46));
    BOOST_CHECK_EQUAL(sweep.amount, 5'000'000);
    BOOST_CHECK_EQUAL(sweep.b3_height, 461U);
    BOOST_CHECK(modern::BridgeWithdrawalLeafV2(sweep));
    // Nothing to sweep, or more than Vault V2 could hold, is no leaf.
    for (const CAmount bad : {CAmount{0}, CAmount{-1}, MAX_MONEY + 1}) {
        BOOST_CHECK(!modern::BridgeWithdrawalLeafV2(modern::MakeVaultSweepWithdrawalV2(
            v2.vault, v2.successor->asset_id, v2.successor->entry.vault_address, 3, bad, 461)));
    }
}

// ------------------------------------------ V2 deposit leg (node changes 4)

namespace {

std::vector<unsigned char> Blob(const size_t size, const unsigned char fill = 0xab)
{
    return std::vector<unsigned char>(size, fill);
}

//! A MINT record whose receipt proof is exactly `mpt_nodes`.
bridge::BridgeRecordV1 MintWithProof(std::vector<std::vector<unsigned char>> mpt_nodes)
{
    bridge::BridgeMintV1 mint;
    mint.registry_id = Fill(0x31);
    mint.output_index = 0;
    mint.finalized_anchor_hash = Fill(0x32);
    mint.target_block_number = 22'000'000;
    mint.tx_index = 5;
    mint.receipt_log_index = 1;
    mint.ancestry_headers = {Blob(600, 0xf9)};
    mint.mpt_nodes = std::move(mpt_nodes);
    return bridge::BridgeRecordV1{bridge::BridgeRecordKindV1::MINT, std::move(mint)};
}

//! `count` inner nodes of `size` bytes, then one terminal node of `terminal` bytes.
std::vector<std::vector<unsigned char>> Proof(const size_t count, const size_t size, const size_t terminal)
{
    std::vector<std::vector<unsigned char>> out(count, Blob(size, 0xa0));
    out.push_back(Blob(terminal, 0xa1));
    return out;
}

//! The record round-trips canonically under the V2 limits only: the V1
//! codec refuses both the record and its V2 wire bytes.
void CheckV2OnlyRoundTrip(const bridge::BridgeRecordV1& record)
{
    using namespace bridge;
    const auto encoded{EncodeBridgeRecordV1(record, BRIDGE_MPT_LIMITS_V2)};
    BOOST_REQUIRE(encoded);
    BOOST_CHECK_LE(encoded->size(), MAX_BRIDGE_RECORD_SIZE);
    const auto decoded{DecodeBridgeRecordV1(*encoded, BRIDGE_MPT_LIMITS_V2)};
    BOOST_REQUIRE(decoded);
    BOOST_CHECK(*decoded == record);
    BOOST_CHECK(EncodeBridgeRecordV1(*decoded, BRIDGE_MPT_LIMITS_V2) == encoded);
    const auto mpa{MakeBridgeMpaRecord(record, BRIDGE_MPT_LIMITS_V2)};
    BOOST_REQUIRE(mpa);
    BOOST_CHECK(mpa->payload == *encoded);
    BOOST_CHECK(DecodeBridgeMpaRecordV1(*mpa, BRIDGE_MPT_LIMITS_V2) == decoded);

    BOOST_CHECK(!EncodeBridgeRecordV1(record));
    BOOST_CHECK(!DecodeBridgeRecordV1(*encoded));
    BOOST_CHECK(!MakeBridgeMpaRecord(record));
    BOOST_CHECK(!DecodeBridgeMpaRecordV1(*mpa));
}

//! Wire bytes of `record` produced under wider custom limits, for checking
//! that the V2 decoder itself (not only the encoder) refuses them.
std::vector<unsigned char> WireUnder(const bridge::BridgeRecordV1& record,
                                     const bridge::BridgeMptLimits& limits)
{
    const auto encoded{bridge::EncodeBridgeRecordV1(record, limits)};
    BOOST_REQUIRE(encoded);
    return *encoded;
}

std::array<unsigned char, 32> Uint256Amount(uint64_t value)
{
    std::array<unsigned char, 32> out{};
    for (int i{31}; i >= 24; --i) {
        out[i] = static_cast<unsigned char>(value & 0xff);
        value >>= 8;
    }
    return out;
}

bridge::RecipientV1 Recipient(const unsigned char first)
{
    bridge::RecipientV1 out;
    for (size_t i{0}; i < out.pubkey_hash.size(); ++i) {
        out.pubkey_hash[i] = static_cast<unsigned char>(first + i);
    }
    return out;
}

//! B3DepositVault-shaped `Deposit` log (bridge/deposit.h).
std::vector<unsigned char> DepositLog(const bridge::EthAddress& vault, const uint64_t id,
                                      const bridge::EthAddress& token, const uint64_t amount,
                                      const bridge::RecipientV1& recipient)
{
    using namespace bridge;
    uint256 id_topic;
    for (int i{0}; i < 8; ++i) *(id_topic.begin() + 24 + i) = static_cast<unsigned char>(id >> (8 * (7 - i)));
    uint256 token_topic;
    std::copy(token.begin(), token.end(), token_topic.begin() + 12);
    std::vector<unsigned char> data;
    const auto amount_word{Uint256Amount(amount)};
    const auto recipient_word{EncodeRecipientV1(recipient)};
    data.insert(data.end(), amount_word.begin(), amount_word.end());
    data.insert(data.end(), recipient_word.begin(), recipient_word.end());
    std::vector<std::vector<unsigned char>> topics;
    for (const uint256& t : std::array<uint256, 3>{DepositTopic(), id_topic, token_topic}) {
        topics.push_back(RlpEncodeBytes({t.begin(), 32}));
    }
    return RlpEncodeList({RlpEncodeBytes(vault), RlpEncodeList(topics), RlpEncodeBytes(data)});
}

//! Successful post-Byzantium receipt body; `extra_field` appends a fifth field.
std::vector<unsigned char> ReceiptBody(const std::vector<std::vector<unsigned char>>& logs,
                                       const bool extra_field = false)
{
    using namespace bridge;
    std::vector<std::vector<unsigned char>> fields{
        RlpEncodeUint64(1), RlpEncodeUint64(21'000), RlpEncodeBytes(std::vector<unsigned char>(256, 0x00)),
        RlpEncodeList(logs)};
    if (extra_field) fields.push_back(RlpEncodeUint64(7));
    return RlpEncodeList(fields);
}

std::vector<unsigned char> Prefixed(const unsigned char first, const std::vector<unsigned char>& body)
{
    std::vector<unsigned char> out{first};
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

//! A deposit matching RegtestV2Params()'s vault entry exactly.
bridge::ProvenBridgeDeposit MatchingV2Deposit(const Consensus::BridgeV2Params& params)
{
    bridge::ProvenBridgeDeposit out;
    out.origin_chain_id = params.vault.origin_chain_id;
    out.execution_block_number = params.vault.origin_deployment_block;
    out.vault_address = params.vault.vault_address;
    out.event.deposit_id = 17;
    out.event.token = params.vault.token_address;
    out.event.amount = Uint256Amount(1'250'000);
    out.event.b3_recipient = bridge::EncodeRecipientV1(Recipient(0x80));
    return out;
}

void CheckClearedAuthorization(const bridge::BridgeMintAuthorization& out)
{
    BOOST_CHECK(out.asset.IsNull());
    BOOST_CHECK_EQUAL(out.amount, 0);
    BOOST_CHECK(out.recipient_script.empty());
    BOOST_CHECK(out.nullifier == bridge::BridgeDepositKey{});
}

} // namespace

BOOST_AUTO_TEST_CASE(receipt_limits_v2)
{
    using namespace bridge;
    using Nodes = std::vector<std::vector<unsigned char>>;
    BOOST_CHECK_EQUAL(BRIDGE_MPT_LIMITS_V1.max_node_size, 1'024U);
    BOOST_CHECK_EQUAL(BRIDGE_MPT_LIMITS_V1.max_terminal_node_size, 1'024U);
    BOOST_CHECK_EQUAL(BRIDGE_MPT_LIMITS_V1.max_total_bytes, 11U * 1'024);
    BOOST_CHECK_EQUAL(BRIDGE_MPT_LIMITS_V2.max_node_size, 1'024U);
    BOOST_CHECK_EQUAL(BRIDGE_MPT_LIMITS_V2.max_terminal_node_size, 8'192U);
    BOOST_CHECK_EQUAL(BRIDGE_MPT_LIMITS_V2.max_total_bytes, 16'384U);

    // Inside V1's limits both sets produce and accept the same bytes.
    const BridgeRecordV1 small{MintWithProof(Proof(2, 1'024, 1'024))};
    const auto v1_wire{EncodeBridgeRecordV1(small)};
    BOOST_REQUIRE(v1_wire);
    BOOST_CHECK(EncodeBridgeRecordV1(small, BRIDGE_MPT_LIMITS_V2) == v1_wire);
    BOOST_CHECK(DecodeBridgeRecordV1(*v1_wire, BRIDGE_MPT_LIMITS_V2) == small);
    BOOST_CHECK(DecodeBridgeRecordV1(*v1_wire) == small);

    // An 8,192-byte terminal receipt node is V2-only, also as a one-node
    // proof (its only node is terminal) and at 1,025 bytes (V1 + 1).
    CheckV2OnlyRoundTrip(MintWithProof(Proof(2, 532, 8'192)));
    CheckV2OnlyRoundTrip(MintWithProof(Proof(0, 0, 8'192)));
    CheckV2OnlyRoundTrip(MintWithProof(Proof(1, 64, 1'025)));

    // 8,193 bytes is refused by the V2 encoder and decoder.
    const BridgeRecordV1 terminal_over{MintWithProof(Proof(2, 532, 8'193))};
    BOOST_CHECK(!EncodeBridgeRecordV1(terminal_over, BRIDGE_MPT_LIMITS_V2));
    BOOST_CHECK(!DecodeBridgeRecordV1(WireUnder(terminal_over, {1'024, 8'193, 16'384}), BRIDGE_MPT_LIMITS_V2));

    // Inner nodes keep 1,024 bytes: 1,025 is refused, and the large
    // allowance cannot move off the terminal position.
    const BridgeRecordV1 inner_over{MintWithProof(Proof(1, 1'025, 600))};
    BOOST_CHECK(!EncodeBridgeRecordV1(inner_over, BRIDGE_MPT_LIMITS_V2));
    BOOST_CHECK(!DecodeBridgeRecordV1(WireUnder(inner_over, {1'025, 8'192, 16'384}), BRIDGE_MPT_LIMITS_V2));
    const BridgeRecordV1 misplaced{MintWithProof(Nodes{Blob(8'192), Blob(600)})};
    BOOST_CHECK(!EncodeBridgeRecordV1(misplaced, BRIDGE_MPT_LIMITS_V2));
    BOOST_CHECK(!DecodeBridgeRecordV1(WireUnder(misplaced, {8'192, 8'192, 16'384}), BRIDGE_MPT_LIMITS_V2));

    // 8,192 + 8 x 1,024 = 16,384 total is accepted; one more byte is not.
    CheckV2OnlyRoundTrip(MintWithProof(Proof(8, 1'024, 8'192)));
    Nodes over_total_nodes{Proof(8, 1'024, 8'192)};
    over_total_nodes.insert(over_total_nodes.begin(), Blob(1));
    const BridgeRecordV1 over_total{MintWithProof(over_total_nodes)};
    BOOST_CHECK(!EncodeBridgeRecordV1(over_total, BRIDGE_MPT_LIMITS_V2));
    BOOST_CHECK(!DecodeBridgeRecordV1(WireUnder(over_total, {1'024, 8'192, 16'385}), BRIDGE_MPT_LIMITS_V2));

    // Unchanged under V2: the 64-node count, empty nodes, and ancestry
    // headers (the terminal allowance is the receipt proof's only).
    BOOST_CHECK(!EncodeBridgeRecordV1(MintWithProof(Proof(MAX_BRIDGE_MPT_NODES, 1, 1)), BRIDGE_MPT_LIMITS_V2));
    BOOST_CHECK(EncodeBridgeRecordV1(MintWithProof(Proof(MAX_BRIDGE_MPT_NODES - 1, 1, 1)), BRIDGE_MPT_LIMITS_V2));
    BOOST_CHECK(!EncodeBridgeRecordV1(MintWithProof(Nodes{Blob(10), {}}), BRIDGE_MPT_LIMITS_V2));
    {
        auto record{MintWithProof(Proof(0, 0, 100))};
        std::get<BridgeMintV1>(record.payload).ancestry_headers = {Blob(600), Blob(MAX_BRIDGE_RLP_ITEM_SIZE + 1)};
        BOOST_CHECK(!EncodeBridgeRecordV1(record, BRIDGE_MPT_LIMITS_V2));
    }

    // The 32 KiB record bound caps a full ancestry (20 KiB) plus a full V2
    // receipt proof (16 KiB), which V1's static wire bound cannot.
    {
        auto record{MintWithProof(Proof(8, 1'024, 8'192))};
        std::get<BridgeMintV1>(record.payload).ancestry_headers.assign(
            MAX_BRIDGE_EXECUTION_ANCESTRY_BYTES / MAX_BRIDGE_RLP_ITEM_SIZE, Blob(MAX_BRIDGE_RLP_ITEM_SIZE, 0xf9));
        BOOST_CHECK(!EncodeBridgeRecordV1(record, BRIDGE_MPT_LIMITS_V2));
        std::get<BridgeMintV1>(record.payload).mpt_nodes = Proof(0, 0, 8'192);
        CheckV2OnlyRoundTrip(record);
    }

    // Other kinds ignore the limit set: a backfill encodes the same bytes
    // under both.
    const BridgeRecordV1 backfill{BridgeRecordKindV1::EXECUTION_BACKFILL,
                                  BridgeExecutionBackfillV1{Fill(0x33), 22'000'001, {Blob(700), Blob(800)}}};
    const auto backfill_v1{EncodeBridgeRecordV1(backfill)};
    BOOST_REQUIRE(backfill_v1);
    BOOST_CHECK(EncodeBridgeRecordV1(backfill, BRIDGE_MPT_LIMITS_V2) == backfill_v1);
    BOOST_CHECK(DecodeBridgeRecordV1(*backfill_v1, BRIDGE_MPT_LIMITS_V2) == backfill);
}

BOOST_AUTO_TEST_CASE(receipt_types)
{
    using namespace bridge;
    BOOST_CHECK_EQUAL(unsigned{RECEIPT_MAX_TYPE_V1}, 0x04U);
    BOOST_CHECK_EQUAL(unsigned{RECEIPT_MAX_TYPE_V2}, 0x7fU);

    const EthAddress vault{FillAddress(0x44)};
    const EthAddress token{FillAddress(0x55)};
    const auto body{ReceiptBody({DepositLog(vault, 9, token, 1'000'000, Recipient(0x80))})};

    // Legacy receipts and types 1..4 decode under both rule sets, as today.
    for (const uint8_t max : {RECEIPT_MAX_TYPE_V1, RECEIPT_MAX_TYPE_V2, uint8_t{0xff}}) {
        const auto legacy{DecodeReceipt(body, max)};
        BOOST_REQUIRE(legacy);
        BOOST_CHECK_EQUAL(unsigned{legacy->type}, 0U);
        BOOST_CHECK_EQUAL(legacy->logs.size(), 1U);
    }
    BOOST_CHECK(DecodeReceipt(body));
    for (unsigned type{1}; type <= unsigned{RECEIPT_MAX_TYPE_V1}; ++type) {
        const auto typed{Prefixed(static_cast<unsigned char>(type), body)};
        const auto v1{DecodeReceipt(typed)};
        const auto v2{DecodeReceipt(typed, RECEIPT_MAX_TYPE_V2)};
        BOOST_REQUIRE(v1 && v2);
        BOOST_CHECK_EQUAL(unsigned{v1->type}, type);
        BOOST_CHECK_EQUAL(unsigned{v2->type}, type);
    }

    // Types 0x05..0x7f decode only under the V2 bound.
    for (unsigned type{RECEIPT_MAX_TYPE_V1 + 1U}; type <= unsigned{RECEIPT_MAX_TYPE_V2}; ++type) {
        const auto typed{Prefixed(static_cast<unsigned char>(type), body)};
        BOOST_CHECK_MESSAGE(!DecodeReceipt(typed), "type " << type << " decoded under V1");
        const auto v2{DecodeReceipt(typed, RECEIPT_MAX_TYPE_V2)};
        BOOST_REQUIRE_MESSAGE(v2, "type " << type << " refused under V2");
        BOOST_CHECK_EQUAL(unsigned{v2->type}, type);
        BOOST_CHECK(v2->status);
        BOOST_CHECK_EQUAL(v2->logs.size(), 1U);
    }

    // 0x00 and 0x80..0xbf are no receipt under any bound; a bound above 0x7f
    // is clamped to EIP-2718's ceiling.
    for (const unsigned first : {0x00U, 0x80U, 0x81U, 0xb7U, 0xb8U, 0xbfU}) {
        const auto bad{Prefixed(static_cast<unsigned char>(first), body)};
        BOOST_CHECK(!DecodeReceipt(bad));
        BOOST_CHECK(!DecodeReceipt(bad, RECEIPT_MAX_TYPE_V2));
        BOOST_CHECK(!DecodeReceipt(bad, 0xff));
    }
    BOOST_CHECK(!DecodeReceipt(std::vector<unsigned char>{0x00}, RECEIPT_MAX_TYPE_V2));
    BOOST_CHECK(!DecodeReceipt(std::vector<unsigned char>{0x80}, RECEIPT_MAX_TYPE_V2));

    // A typed envelope must still carry the four-field receipt list.
    BOOST_CHECK(!DecodeReceipt(std::vector<unsigned char>{0x05}, RECEIPT_MAX_TYPE_V2));
    BOOST_CHECK(!DecodeReceipt(Prefixed(0x05, ReceiptBody({}, /*extra_field=*/true)), RECEIPT_MAX_TYPE_V2));
    BOOST_CHECK(!DecodeReceipt(Prefixed(0x05, Prefixed(0x02, body)), RECEIPT_MAX_TYPE_V2));
    BOOST_CHECK(!DecodeReceipt(Prefixed(0x05, RlpEncodeBytes(body)), RECEIPT_MAX_TYPE_V2));

    // The vault's Deposit event extracts unchanged from a type-0x05 receipt.
    const auto receipt{DecodeReceipt(Prefixed(0x05, body), RECEIPT_MAX_TYPE_V2)};
    BOOST_REQUIRE(receipt);
    const auto event{ExtractDepositAt(*receipt, vault, 0)};
    BOOST_REQUIRE(event);
    BOOST_CHECK_EQUAL(event->deposit_id, 9U);
    BOOST_CHECK(event->token == token);
    BOOST_CHECK(event->amount == Uint256Amount(1'000'000));
    BOOST_CHECK(event->b3_recipient == EncodeRecipientV1(Recipient(0x80)));
}

BOOST_AUTO_TEST_CASE(nullifier_namespace)
{
    using namespace bridge;
    BOOST_CHECK_EQUAL(unsigned{BRIDGE_NULLIFIER_NAMESPACE_V1}, 1U);
    BOOST_CHECK_EQUAL(unsigned{BRIDGE_NULLIFIER_NAMESPACE_V2}, 2U);
    BOOST_CHECK(BridgeDepositKey{}.nullifier_namespace == BRIDGE_NULLIFIER_NAMESPACE_V1);

    const EthAddress vault{FillAddress(0x44)};
    // A three-field (V1) initialiser keeps namespace 1.
    const BridgeDepositKey v1{31'337, vault, 9};
    BOOST_CHECK(v1.nullifier_namespace == BRIDGE_NULLIFIER_NAMESPACE_V1);
    const BridgeDepositKey v2{31'337, vault, 9, BRIDGE_NULLIFIER_NAMESPACE_V2};

    // Equal but for the namespace: distinct keys, both held by one set.
    BOOST_CHECK(v1 != v2);
    BOOST_CHECK(v1 < v2);
    BOOST_CHECK(!(v2 < v1));
    std::set<BridgeDepositKey> keys{v1, v2};
    BOOST_CHECK_EQUAL(keys.size(), 2U);
    BOOST_CHECK(keys.contains(v1) && keys.contains(v2));

    // The namespace is the last tie element: V1 keys keep their relative
    // order, and a later deposit id sorts after either namespace.
    const BridgeDepositKey v1_next{31'337, vault, 10};
    BOOST_CHECK(v2 < v1_next);
    BOOST_CHECK(v1 < v1_next);

    BridgeNullifierSet set;
    BridgeNullifierUndo undo;
    const std::array batch{v1, v2};
    BOOST_REQUIRE(set.ApplyBlock(batch, undo));
    BOOST_CHECK_EQUAL(set.Size(), 2U);
    BOOST_CHECK(set.Contains(v1) && set.Contains(v2));
    // Replay inside one namespace is still refused.
    BridgeNullifierUndo replay;
    BOOST_CHECK(!set.ApplyBlock(std::span<const BridgeDepositKey>{batch}.last(1), replay));
    BOOST_CHECK(set.UndoBlock(undo));
    BOOST_CHECK_EQUAL(set.Size(), 0U);

    // V1 admission keeps namespace 1.
    BridgeAssetRegistryEntry registry;
    registry.origin_chain_id = 1;
    registry.origin_deployment_block = 1'000;
    registry.vault_address = vault;
    registry.token_address = FillAddress(0x55);
    registry.b3_asset_id = uint256::ONE;
    registry.origin_decimals = 6;
    registry.asset_decimals = 6;
    registry.vault_runtime_code_hash = uint256::ONE;
    registry.implementation_or_adapter = uint256::ONE;
    registry.adapter_version = Consensus::BRIDGE_ADAPTER_VERSION_DIRECT_TOKEN_V1;
    registry.approval_first_height = 100;
    registry.state = BridgeRegistryState::ACTIVE;
    ProvenBridgeDeposit deposit;
    deposit.origin_chain_id = 1;
    deposit.execution_block_number = 1'000;
    deposit.vault_address = vault;
    deposit.event.deposit_id = 9;
    deposit.event.token = registry.token_address;
    deposit.event.amount = Uint256Amount(5);
    deposit.event.b3_recipient = EncodeRecipientV1(Recipient(0x80));
    BridgeMintAuthorization mint;
    BOOST_REQUIRE(AdmitProvenDeposit(registry, deposit, 150, mint) == BridgeAdmissionResult::OK);
    BOOST_CHECK(mint.nullifier == (BridgeDepositKey{1, vault, 9}));
    BOOST_CHECK(mint.nullifier.nullifier_namespace == BRIDGE_NULLIFIER_NAMESPACE_V1);
}

BOOST_AUTO_TEST_CASE(admit_deposit_v2)
{
    using namespace bridge;
    const Consensus::BridgeV2Params params{RegtestV2Params()};
    BOOST_REQUIRE(params.Valid());
    const modern::AssetId asset{modern::BridgeAssetIdV2(Fill(0x5d), params.vault)};
    const ProvenBridgeDeposit deposit{MatchingV2Deposit(params)};
    const int32_t v{params.activation_height};
    const BridgeMintBudget none{};
    BridgeMintAuthorization mint;

    BOOST_REQUIRE(AdmitDepositV2(params, asset, deposit, v, none, mint) == BridgeAdmissionResult::OK);
    BOOST_CHECK(mint.asset == asset);
    BOOST_CHECK_EQUAL(mint.amount, 1'250'000);
    BOOST_CHECK(mint.recipient_script == RecipientV1Script(Recipient(0x80)));
    BOOST_CHECK(mint.nullifier ==
                (BridgeDepositKey{31'337, params.vault.vault_address, 17, BRIDGE_NULLIFIER_NAMESPACE_V2}));
    BOOST_CHECK(mint.nullifier.nullifier_namespace == BRIDGE_NULLIFIER_NAMESPACE_V2);
    BOOST_CHECK(AdmitDepositV2(params, asset, deposit, std::numeric_limits<int32_t>::max(), none, mint) ==
                BridgeAdmissionResult::OK);

    const auto expect{[&](const Consensus::BridgeV2Params& p, const modern::AssetId& a,
                          const ProvenBridgeDeposit& d, const int32_t height, const BridgeMintBudget& used,
                          const BridgeAdmissionResult want) {
        BridgeMintAuthorization out;
        out.amount = 99; // stale output must be cleared on refusal
        const BridgeAdmissionResult got{AdmitDepositV2(p, a, d, height, used, out)};
        BOOST_CHECK(got == want);
        if (want != BridgeAdmissionResult::OK) CheckClearedAuthorization(out);
    }};
    using R = BridgeAdmissionResult;

    // Inactive below V or with an incomplete envelope; unnamed asset.
    expect(params, asset, deposit, v - 1, none, R::REGISTRY_INACTIVE);
    expect(params, asset, deposit, 0, none, R::REGISTRY_INACTIVE);
    Consensus::BridgeV2Params incomplete{params};
    incomplete.vault.origin_deployment_block = 0;
    expect(incomplete, asset, deposit, v, none, R::REGISTRY_INACTIVE);
    incomplete = params;
    incomplete.verifier.address = {};
    expect(incomplete, asset, deposit, v, none, R::REGISTRY_INACTIVE);
    expect(params, uint256{}, deposit, v, none, R::CONFIGURATION_INCOMPLETE);

    // Tuple mismatches, in V1's order.
    ProvenBridgeDeposit changed{deposit};
    --changed.execution_block_number;
    expect(params, asset, changed, v, none, R::BEFORE_DEPLOYMENT);
    changed = deposit;
    changed.origin_chain_id = 1;
    expect(params, asset, changed, v, none, R::ORIGIN_MISMATCH);
    changed = deposit;
    changed.vault_address[0] ^= 1;
    expect(params, asset, changed, v, none, R::VAULT_MISMATCH);
    changed = deposit;
    changed.event.token[0] ^= 1;
    expect(params, asset, changed, v, none, R::TOKEN_MISMATCH);
    changed = deposit;
    changed.event.b3_recipient = {};
    expect(params, asset, changed, v, none, R::RECIPIENT_INVALID);
    changed = deposit;
    changed.event.amount = {};
    expect(params, asset, changed, v, none, R::AMOUNT_INVALID);
    changed = deposit;
    changed.event.amount[0] = 1; // far above MAX_MONEY
    expect(params, asset, changed, v, none, R::AMOUNT_INVALID);

    // Caps: exact fills pass; one raw unit over, a negative or an already
    // exceeded budget is refused.
    const CAmount block_cap{params.vault.mint_caps.max_per_block};
    expect(params, asset, deposit, v, {block_cap - 1'250'000, 0}, R::OK);
    expect(params, asset, deposit, v, {block_cap - 1'250'000 + 1, 0}, R::BLOCK_CAP_EXCEEDED);
    expect(params, asset, deposit, v, {-1, 0}, R::BLOCK_CAP_EXCEEDED);
    expect(params, asset, deposit, v, {block_cap + 1, 0}, R::BLOCK_CAP_EXCEEDED);
    Consensus::BridgeV2Params wide_epoch{params};
    wide_epoch.vault.mint_caps.max_per_epoch = 2 * block_cap;
    BOOST_REQUIRE(wide_epoch.Valid());
    const CAmount epoch_cap{wide_epoch.vault.mint_caps.max_per_epoch};
    expect(wide_epoch, asset, deposit, v, {0, epoch_cap - 1'250'000}, R::OK);
    expect(wide_epoch, asset, deposit, v, {0, epoch_cap - 1'250'000 + 1}, R::EPOCH_CAP_EXCEEDED);
    expect(wide_epoch, asset, deposit, v, {0, -1}, R::EPOCH_CAP_EXCEEDED);
    expect(wide_epoch, asset, deposit, v, {0, epoch_cap + 1}, R::EPOCH_CAP_EXCEEDED);

    // V2 admission needs no vault code hash (OI-3: the regtest envelope
    // leaves it unset), and an Ethereum-mainnet-shaped envelope (every pin
    // set) admits a USDT deposit into its vault the same way.
    const Consensus::BridgeV2Params mainnet{MainnetShapedV2Params()};
    BOOST_REQUIRE(mainnet.Valid());
    BOOST_REQUIRE(!params.vault.vault_runtime_code_hash);
    const ProvenBridgeDeposit mainnet_deposit{MatchingV2Deposit(mainnet)};
    BOOST_CHECK(AdmitDepositV2(mainnet, asset, mainnet_deposit, v, none, mint) == R::OK);
    BOOST_CHECK(mint.nullifier == (BridgeDepositKey{1, mainnet.vault.vault_address, 17, BRIDGE_NULLIFIER_NAMESPACE_V2}));
    expect(mainnet, asset, deposit, v, none, R::ORIGIN_MISMATCH);
}

BOOST_AUTO_TEST_CASE(admit_deposit_v2_successor_entry)
{
    // AdmitDepositV2 by vault entry (node changes sections 4 and 7.6): the
    // B-23 successor v' admits its own vault's deposits from h_m, under its
    // own nullifier vault, minting v's bUSD V2 AssetId within the caps the
    // two entries share.
    using namespace bridge;
    using R = BridgeAdmissionResult;
    using Consensus::BridgeV2EntryRole;
    const uint256 domain{FixtureDomain()};
    const Consensus::BridgeV2Params params{WithSuccessor(RegtestV2Params(), domain)};
    BOOST_REQUIRE(params.Valid());
    const Consensus::BridgeV2VaultEntry& launch{*params.Entry(BridgeV2EntryRole::LAUNCH)};
    const Consensus::BridgeV2VaultEntry& next{*params.Entry(BridgeV2EntryRole::SUCCESSOR)};
    BOOST_REQUIRE(&launch == &params.vault);
    BOOST_REQUIRE(&next == &params.successor->entry);
    const modern::AssetId asset{modern::BridgeAssetIdV2(domain, params.vault)};
    BOOST_REQUIRE(asset == params.successor->asset_id);
    const int32_t v{params.activation_height};
    const int32_t h_m{params.successor->migration_height};
    BOOST_REQUIRE(h_m > v);
    const BridgeMintBudget none{};
    constexpr CAmount AMOUNT{1'250'000};

    // The same deposit id (17) into each vault, each at its deployment block.
    const ProvenBridgeDeposit launch_deposit{MatchingV2Deposit(params)};
    ProvenBridgeDeposit successor_deposit{launch_deposit};
    successor_deposit.vault_address = next.vault_address;
    successor_deposit.execution_block_number = next.origin_deployment_block;
    BOOST_REQUIRE(next.origin_deployment_block > launch.origin_deployment_block);

    const auto expect{[&](const Consensus::BridgeV2Params& p, const Consensus::BridgeV2VaultEntry& e,
                          const ProvenBridgeDeposit& d, const int32_t height,
                          const BridgeMintBudget& used, const R want) {
        BridgeMintAuthorization out;
        out.amount = 99; // stale output must be cleared on refusal
        const R got{AdmitDepositV2(p, e, asset, d, height, used, out)};
        BOOST_CHECK(got == want);
        if (want != R::OK) CheckClearedAuthorization(out);
    }};

    // From h_m a v' deposit is admitted: the shared asset, nullifier vault v'.
    BridgeMintAuthorization successor_mint;
    BOOST_REQUIRE(AdmitDepositV2(params, next, asset, successor_deposit, h_m, none, successor_mint) == R::OK);
    BOOST_CHECK(successor_mint.asset == asset);
    BOOST_CHECK_EQUAL(successor_mint.amount, AMOUNT);
    BOOST_CHECK(successor_mint.recipient_script == RecipientV1Script(Recipient(0x80)));
    BOOST_CHECK(successor_mint.nullifier ==
                (BridgeDepositKey{31'337, next.vault_address, 17, BRIDGE_NULLIFIER_NAMESPACE_V2}));
    expect(params, next, successor_deposit, std::numeric_limits<int32_t>::max(), none, R::OK);

    // The retired launch entry still admits v deposits after h_m (v cannot
    // refund; B3 mints and sweeps them), under v's nullifier: the same id in
    // the two vaults gives two distinct nullifiers. The launch overload is
    // exactly the launch entry.
    BridgeMintAuthorization launch_mint;
    BOOST_REQUIRE(AdmitDepositV2(params, launch, asset, launch_deposit, h_m, none, launch_mint) == R::OK);
    BOOST_CHECK(launch_mint.asset == asset);
    BOOST_CHECK(launch_mint.nullifier ==
                (BridgeDepositKey{31'337, launch.vault_address, 17, BRIDGE_NULLIFIER_NAMESPACE_V2}));
    BOOST_CHECK(launch_mint.nullifier != successor_mint.nullifier);
    BridgeMintAuthorization overload_mint;
    BOOST_REQUIRE(AdmitDepositV2(params, asset, launch_deposit, h_m, none, overload_mint) == R::OK);
    BOOST_CHECK(overload_mint.asset == launch_mint.asset);
    BOOST_CHECK_EQUAL(overload_mint.amount, launch_mint.amount);
    BOOST_CHECK(overload_mint.recipient_script == launch_mint.recipient_script);
    BOOST_CHECK(overload_mint.nullifier == launch_mint.nullifier);

    // The successor entry is inactive below h_m (and below V), while the
    // launch entry is active from V.
    expect(params, next, successor_deposit, h_m - 1, none, R::REGISTRY_INACTIVE);
    expect(params, next, successor_deposit, v, none, R::REGISTRY_INACTIVE);
    expect(params, next, successor_deposit, v - 1, none, R::REGISTRY_INACTIVE);
    expect(params, launch, launch_deposit, v, none, R::OK);
    expect(params, launch, launch_deposit, v - 1, none, R::REGISTRY_INACTIVE);

    // A launch-vault event is refused for the successor entry, and a
    // successor-vault event for the launch entry (either overload).
    ProvenBridgeDeposit late_launch{launch_deposit};
    late_launch.execution_block_number = next.origin_deployment_block;
    expect(params, next, late_launch, h_m, none, R::VAULT_MISMATCH);
    expect(params, launch, successor_deposit, h_m, none, R::VAULT_MISMATCH);
    BridgeMintAuthorization refused;
    BOOST_CHECK(AdmitDepositV2(params, asset, successor_deposit, h_m, none, refused) == R::VAULT_MISMATCH);
    CheckClearedAuthorization(refused);
    // v' has its own deployment block (v's + 10), and v's chain and token.
    expect(params, next, launch_deposit, h_m, none, R::BEFORE_DEPLOYMENT);
    ProvenBridgeDeposit changed{successor_deposit};
    --changed.execution_block_number;
    expect(params, next, changed, h_m, none, R::BEFORE_DEPLOYMENT);
    changed = successor_deposit;
    changed.origin_chain_id = 1;
    expect(params, next, changed, h_m, none, R::ORIGIN_MISMATCH);
    changed = successor_deposit;
    changed.event.token[0] ^= 1;
    expect(params, next, changed, h_m, none, R::TOKEN_MISMATCH);
    changed = successor_deposit;
    changed.event.b3_recipient = {};
    expect(params, next, changed, h_m, none, R::RECIPIENT_INVALID);
    BridgeMintAuthorization unnamed;
    BOOST_CHECK(AdmitDepositV2(params, next, uint256{}, successor_deposit, h_m, none, unnamed) ==
                R::CONFIGURATION_INCOMPLETE);
    CheckClearedAuthorization(unnamed);

    // Caps are shared: one budget, keyed by the shared AssetId, covers both
    // entries' mints in a block or epoch, and the caps are equal.
    const CAmount block_cap{next.mint_caps.max_per_block};
    BOOST_REQUIRE_EQUAL(block_cap, launch.mint_caps.max_per_block);
    // A launch mint has used block_cap - AMOUNT of the block: a v' mint of
    // AMOUNT fills it exactly, one more raw unit is refused for either entry.
    expect(params, next, successor_deposit, h_m, {block_cap - AMOUNT, 0}, R::OK);
    expect(params, next, successor_deposit, h_m, {block_cap - AMOUNT + 1, 0}, R::BLOCK_CAP_EXCEEDED);
    expect(params, launch, launch_deposit, h_m, {block_cap - AMOUNT + 1, 0}, R::BLOCK_CAP_EXCEEDED);
    // Widening the epoch cap needs both entries (SuccessorValid refuses one
    // entry alone, and an invalid envelope admits nothing).
    Consensus::BridgeV2Params lopsided{params};
    lopsided.successor->entry.mint_caps.max_per_epoch = 2 * block_cap;
    BOOST_CHECK(!lopsided.Valid());
    expect(lopsided, *lopsided.Entry(BridgeV2EntryRole::SUCCESSOR), successor_deposit, h_m, none,
           R::REGISTRY_INACTIVE);
    Consensus::BridgeV2Params wide{params};
    wide.vault.mint_caps.max_per_epoch = 2 * block_cap;
    wide.successor->entry.mint_caps.max_per_epoch = 2 * block_cap;
    BOOST_REQUIRE(wide.Valid());
    const CAmount epoch_cap{2 * block_cap};
    const Consensus::BridgeV2VaultEntry& wide_next{*wide.Entry(BridgeV2EntryRole::SUCCESSOR)};
    expect(wide, wide_next, successor_deposit, h_m, {0, epoch_cap - AMOUNT}, R::OK);
    expect(wide, wide_next, successor_deposit, h_m, {0, epoch_cap - AMOUNT + 1}, R::EPOCH_CAP_EXCEEDED);
    expect(wide, wide.vault, launch_deposit, h_m, {0, epoch_cap - AMOUNT + 1}, R::EPOCH_CAP_EXCEEDED);

    // The entry must be one of the envelope's own: an equal copy, another
    // envelope's successor and a launch-only envelope's absent successor are
    // refused as inactive.
    const Consensus::BridgeV2VaultEntry copy{next};
    expect(params, copy, successor_deposit, h_m, none, R::REGISTRY_INACTIVE);
    expect(params, wide_next, successor_deposit, h_m, none, R::REGISTRY_INACTIVE);
    expect(wide, next, successor_deposit, h_m, none, R::REGISTRY_INACTIVE);
    const Consensus::BridgeV2Params launch_only{RegtestV2Params()};
    BOOST_REQUIRE(launch_only.Valid());
    BOOST_CHECK(!launch_only.Entry(BridgeV2EntryRole::SUCCESSOR));
    expect(launch_only, next, successor_deposit, h_m, none, R::REGISTRY_INACTIVE);
    expect(launch_only, launch_only.vault, launch_deposit, h_m, none, R::OK);

    // Without a successor the entry form is the launch overload, result for
    // result and output for output.
    std::vector<ProvenBridgeDeposit> cases{launch_deposit, successor_deposit};
    for (const auto& mutate : std::vector<std::function<void(ProvenBridgeDeposit&)>>{
             [](ProvenBridgeDeposit& d) { --d.execution_block_number; },
             [](ProvenBridgeDeposit& d) { d.origin_chain_id = 1; },
             [](ProvenBridgeDeposit& d) { d.event.token[0] ^= 1; },
             [](ProvenBridgeDeposit& d) { d.event.amount = {}; },
             [](ProvenBridgeDeposit& d) { d.event.b3_recipient = {}; },
         }) {
        ProvenBridgeDeposit d{launch_deposit};
        mutate(d);
        cases.push_back(d);
    }
    for (const auto& d : cases) {
        for (const int32_t height : {v - 1, v, h_m}) {
            for (const BridgeMintBudget& used : {none, BridgeMintBudget{block_cap - AMOUNT + 1, 0}}) {
                BridgeMintAuthorization by_entry;
                BridgeMintAuthorization by_overload;
                const R a{AdmitDepositV2(launch_only, launch_only.vault, asset, d, height, used, by_entry)};
                const R b{AdmitDepositV2(launch_only, asset, d, height, used, by_overload)};
                BOOST_CHECK(a == b);
                BOOST_CHECK(by_entry.asset == by_overload.asset);
                BOOST_CHECK_EQUAL(by_entry.amount, by_overload.amount);
                BOOST_CHECK(by_entry.recipient_script == by_overload.recipient_script);
                BOOST_CHECK(by_entry.nullifier == by_overload.nullifier);
            }
        }
    }
}

// ---------------------------- fresh-lineage root hook (node changes 7.4)

namespace {

//! A distinct stand-in block hash per height.
uint256 HookBlockHash(const int height)
{
    uint256 out{Fill(0xb3)};
    out.data()[0] = static_cast<unsigned char>(height);
    out.data()[1] = static_cast<unsigned char>(height >> 8);
    out.data()[2] = static_cast<unsigned char>(height >> 16);
    return out;
}

/**
 * Connect `height` as the next block of `index`: an empty bridge block, or
 * one whose only effects are the V2 burns `withdrawals` (their ids continue
 * the V2 tree and their b3_height is `height`). The delta is built the way
 * VerifyBlock builds it (store chosen by height, V1 tree untouched) and is
 * still validated by ConnectBlock.
 */
void ConnectHookBlock(node::BridgeStateIndex& index, const Consensus::Params& params, const int height,
                      const std::vector<modern::BridgeWithdrawalV2>& withdrawals = {})
{
    node::BridgeBlockDelta delta;
    delta.height = height;
    delta.block_hash = HookBlockHash(height);
    delta.previous_height = index.ConnectedHeight();
    delta.previous_block_hash = index.ConnectedHash();
    delta.withdrawal_tree_before = index.WithdrawalTree();
    delta.withdrawal_tree_after = index.WithdrawalTree();
    delta.v2_store = params.BridgeV2RulesActive(height);
    delta.withdrawal_tree_v2_before = index.WithdrawalTreeV2();
    delta.withdrawal_tree_v2_after = index.WithdrawalTreeV2();
    // The reserve ledger must record the burns (node changes section 7.1):
    // fund the entry first, as the deposits this hook skips would have.
    CAmount burned{0};
    for (const modern::BridgeWithdrawalV2& withdrawal : withdrawals) burned += withdrawal.amount;
    if (burned > 0) {
        bridge::ReserveLedgerV2 funded{index.ReserveLedgerV2()};
        funded.minted += burned;
        node::BridgeStateIndexTestAccess::SeedReserve(index, funded, index.ProvenDepositsV2());
    }
    delta.reserve_v2_before = index.ReserveLedgerV2();
    delta.reserve_v2_after = index.ReserveLedgerV2();
    delta.reserve_v2_after.burned += burned;
    delta.observations_v2_before = index.ObservationsV2();
    delta.conflict_v2_before = index.ConflictV2();
    delta.conflict_v2_after = index.ConflictV2();
    for (size_t i{0}; i < withdrawals.size(); ++i) {
        node::BridgeWithdrawalV2Request request;
        uint256 txid{Fill(0x7a)};
        txid.data()[0] = static_cast<unsigned char>(i);
        txid.data()[1] = static_cast<unsigned char>(height);
        txid.data()[2] = static_cast<unsigned char>(height >> 8);
        txid.data()[3] = static_cast<unsigned char>(height >> 16);
        request.transaction_id = Txid::FromUint256(txid);
        request.burn_output_index = 0;
        request.withdrawal = withdrawals[i];
        const auto leaf{modern::AppendBridgeWithdrawalV2(delta.withdrawal_tree_v2_after, request.withdrawal)};
        BOOST_REQUIRE(leaf);
        request.leaf = *leaf;
        request.connected_height = height;
        request.connected_block = delta.block_hash;
        delta.withdrawals_v2_added.push_back(request);
    }
    std::string error;
    BOOST_REQUIRE_MESSAGE(index.ConnectBlock(delta, error), error);
}

} // namespace

BOOST_AUTO_TEST_CASE(fresh_hook_signs_v2_root_from_v)
{
    const std::optional<uint256> zero{uint256{}};
    const Consensus::BridgeV2Params v2{RegtestV2Params()};
    const int v{v2.activation_height};
    const Consensus::Params params{ConsensusWith(v2)};
    BOOST_REQUIRE(!params.BridgeV2RulesActive(v - 1));
    BOOST_REQUIRE(params.BridgeV2RulesActive(v));
    BOOST_REQUIRE(params.BridgeV2BurnRulesActive(v)); // W2 = V
    const auto asset{modern::ConfiguredBridgeV2AssetId(params)};
    BOOST_REQUIRE(asset);
    const uint256 empty_root{modern::WithdrawalZeroHashes()[modern::WITHDRAWAL_TREE_DEPTH]};
    BOOST_REQUIRE(!empty_root.IsNull());

    // V - 2 .. V: empty blocks. Below V the index records no V2 root and
    // certificates sign zero, with or without an index.
    node::BridgeStateIndex index;
    for (int h{v - 2}; h <= v; ++h) ConnectHookBlock(index, params, h);
    for (int h{v - 2}; h < v; ++h) {
        BOOST_CHECK(!index.WithdrawalRootV2AtHeight(h));
        BOOST_CHECK(node::FreshLineageWithdrawalRoot(h, params, &index) == zero);
        BOOST_CHECK(node::FreshLineageWithdrawalRoot(h, params, nullptr) == zero);
    }
    // From V: the empty depth-32 root (non-zero) before any burn.
    const auto at_v{node::FreshLineageWithdrawalRoot(v, params, &index)};
    BOOST_REQUIRE(at_v);
    BOOST_CHECK(*at_v == empty_root);
    BOOST_CHECK_EQUAL(EvmHex(*at_v), EMPTY_TREE_ROOT);

    // One V2 burn at V + 1 signs the one-leaf root; later heights carry it.
    const int burn_height{v + 1};
    modern::BridgeWithdrawalV2 burn;
    burn.withdrawal_id = 0;
    burn.destination_chain_id = v2.vault.origin_chain_id;
    burn.asset_id = *asset;
    burn.vault = v2.vault.vault_address;
    burn.token = v2.vault.token_address;
    burn.recipient = FillAddress(0x77);
    burn.amount = 1'250'000;
    burn.b3_height = static_cast<uint64_t>(burn_height);
    modern::WithdrawalTreeState one_leaf;
    const auto leaf{modern::AppendBridgeWithdrawalV2(one_leaf, burn)};
    BOOST_REQUIRE(leaf);
    BOOST_REQUIRE(one_leaf.root != empty_root);
    ConnectHookBlock(index, params, burn_height, {burn});
    ConnectHookBlock(index, params, burn_height + 1);
    BOOST_CHECK(index.WithdrawalV2(uint64_t{0}) &&
                index.WithdrawalV2(uint64_t{0})->leaf == *leaf);
    BOOST_CHECK(node::FreshLineageWithdrawalRoot(v, params, &index) == std::optional<uint256>{empty_root});
    BOOST_CHECK(node::FreshLineageWithdrawalRoot(burn_height, params, &index) ==
                std::optional<uint256>{one_leaf.root});
    BOOST_CHECK(node::FreshLineageWithdrawalRoot(burn_height + 1, params, &index) ==
                std::optional<uint256>{one_leaf.root});
    BOOST_CHECK(node::FreshLineageWithdrawalRoot(v - 1, params, &index) == zero);

    // Fail closed from V: no index, or an index that lacks the height (the
    // judge reports fresh-withdrawal-root-unavailable).
    BOOST_CHECK(!node::FreshLineageWithdrawalRoot(v, params, nullptr));
    BOOST_CHECK(!node::FreshLineageWithdrawalRoot(burn_height, params, nullptr));
    BOOST_CHECK(!node::FreshLineageWithdrawalRoot(burn_height + 2, params, &index));
    BOOST_CHECK(!node::FreshLineageWithdrawalRoot(std::numeric_limits<int>::max(), params, &index));

    // Undo: a disconnected burn block's root disappears with it; V keeps its.
    std::string error;
    BOOST_REQUIRE_MESSAGE(index.DisconnectBlock(burn_height + 1, HookBlockHash(burn_height + 1), error), error);
    BOOST_REQUIRE_MESSAGE(index.DisconnectBlock(burn_height, HookBlockHash(burn_height), error), error);
    BOOST_CHECK(!node::FreshLineageWithdrawalRoot(burn_height, params, &index));
    BOOST_CHECK(node::FreshLineageWithdrawalRoot(v, params, &index) == std::optional<uint256>{empty_root});
    BOOST_CHECK_EQUAL(index.WithdrawalV2Count(), 0U);

    // bridgeFlags is 0 on both sides of V without a sticky conflict, and
    // fails closed from V like the root (the conflict cases are in
    // bridge_state_tests/finality_bridge_flags_v2).
    BOOST_CHECK(node::FreshLineageBridgeFlags(v - 1, params, &index) == std::optional<uint8_t>{0});
    BOOST_CHECK(node::FreshLineageBridgeFlags(v - 1, params, nullptr) == std::optional<uint8_t>{0});
    BOOST_CHECK(node::FreshLineageBridgeFlags(v, params, &index) == std::optional<uint8_t>{0});
    BOOST_CHECK(!node::FreshLineageBridgeFlags(v, params, nullptr));
    BOOST_CHECK(!node::FreshLineageBridgeFlags(burn_height, params, &index)); // disconnected above

    // Without a complete bridge V2 envelope (mainnet, testnet, and regtest
    // without a v2 pin section) every height signs zero root and flags 0,
    // index or not.
    Consensus::BridgeV2Params incomplete_v2{v2};
    incomplete_v2.vault.token_runtime_code_hash = uint256{};
    const Consensus::Params incomplete{ConsensusWith(incomplete_v2)};
    const Consensus::Params unset{ConsensusWith(std::nullopt)};
    const std::optional<uint8_t> no_flags{uint8_t{0}};
    for (const int h : {v - 1, v, burn_height, std::numeric_limits<int>::max()}) {
        BOOST_CHECK(node::FreshLineageWithdrawalRoot(h, unset, &index) == zero);
        BOOST_CHECK(node::FreshLineageWithdrawalRoot(h, unset, nullptr) == zero);
        BOOST_CHECK(node::FreshLineageWithdrawalRoot(h, incomplete, &index) == zero);
        BOOST_CHECK(node::FreshLineageWithdrawalRoot(h, incomplete, nullptr) == zero);
        BOOST_CHECK(node::FreshLineageBridgeFlags(h, unset, &index) == no_flags);
        BOOST_CHECK(node::FreshLineageBridgeFlags(h, unset, nullptr) == no_flags);
        BOOST_CHECK(node::FreshLineageBridgeFlags(h, incomplete, &index) == no_flags);
        BOOST_CHECK(node::FreshLineageBridgeFlags(h, incomplete, nullptr) == no_flags);
    }

    // Shared vector: the vector's lineage-2 FinalizedBlockV2 at the bridge
    // activation height carries the eight-leaf withdrawal_tree_v2 root. A V
    // block holding the vector's eight burns (V = W2 = their b3_height)
    // makes B3's hook sign exactly that root.
    Consensus::BridgeV2Params vector_v2{RegtestV2Params()};
    vector_v2.activation_height = 900'000;
    vector_v2.burn_height = 900'000;
    const Consensus::Params vector_params{ConsensusWith(vector_v2)};
    BOOST_REQUIRE(vector_params.BridgeV2RulesActive(900'000));
    node::BridgeStateIndex vector_index;
    ConnectHookBlock(vector_index, vector_params, 899'999);
    ConnectHookBlock(vector_index, vector_params, 900'000, VectorPrefix());
    BOOST_CHECK(node::FreshLineageWithdrawalRoot(899'999, vector_params, &vector_index) == zero);
    const auto vector_root{node::FreshLineageWithdrawalRoot(900'000, vector_params, &vector_index)};
    BOOST_REQUIRE(vector_root);
    BOOST_CHECK_EQUAL(EvmHex(*vector_root), TREE_V2_ROOT);
}

BOOST_AUTO_TEST_SUITE_END()
