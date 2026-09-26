// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.

#include <test/flowmesh_p2fv_bootstrap.h>

#if !defined(ENABLE_FLOWMESH_P2FV_BOOTSTRAP_TEST) || !ENABLE_FLOWMESH_P2FV_BOOTSTRAP_TEST
#error "P2FV bootstrap RPCs require the explicit TEST-only build option"
#endif

#include <chain.h>
#include <chainparams.h>
#include <consensus/era.h>
#include <crypto/common.h>
#include <flowmesh/client_evidence.h>
#include <hash.h>
#include <modern/chain_domain.h>
#include <node/context.h>
#include <node/flowmesh_checkpoint_index.h>
#include <node/flowmesh_service.h>
#include <node/flowmesh_vault_index.h>
#include <node/fn_seat_index.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <sync.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <validation.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

constexpr uint16_t BOOTSTRAP_VERSION{1};
constexpr size_t SEAT_COUNT{4};
constexpr const char* BOOTSTRAP_PROFILE{"TEST-P2FV-fixed4-views0to2-bootstrap-v1"};
constexpr const char* INSTANCE_TAG{"B3/TEST-ONLY/P2FV/REGTEST-BOOTSTRAP/INSTANCE/1"};
constexpr size_t MAX_CERTIFIED_BYTES{flowmesh::FLOWMESH_V1_MAX_MICROBLOCK_BYTES +
    flowmesh::FLOWMESH_CERTIFIED_PAYLOAD_PREFIX_SIZE + flowmesh::FLOWMESH_BLS_CERTIFICATE_MAX_SIZE};

[[noreturn]] void Invalid(const std::string& reason)
{
    throw JSONRPCError(RPC_INVALID_PARAMETER, "P2FV test bootstrap: " + reason);
}

void RequireRegtest()
{
    if (Params().GetChainType() != ChainType::REGTEST) {
        throw JSONRPCError(RPC_MISC_ERROR, "P2FV test bootstrap is available only on regtest");
    }
}

uint256 ExactId(const UniValue& value, const char* name)
{
    if (!value.isStr()) Invalid(std::string{name} + " must be an exact nonzero 64-hex string");
    const std::string& hex{value.get_str()};
    if (hex.size() != 64 || !IsHex(hex)) Invalid(std::string{name} + " must be an exact nonzero 64-hex string");
    const auto out{uint256::FromHex(hex)};
    if (!out || out->IsNull()) Invalid(std::string{name} + " must be nonzero");
    return *out;
}

std::vector<unsigned char> BoundedHex(const UniValue& value, const size_t max_bytes, const char* name)
{
    if (!value.isStr()) Invalid(std::string{name} + " must be a hex string");
    // Check the original string before hex decoding can allocate a byte buffer.
    const std::string& hex{value.get_str()};
    if (hex.empty() || hex.size() > max_bytes * 2 || !IsHex(hex)) {
        Invalid(std::string{name} + " is empty, malformed, or exceeds its byte bound");
    }
    return ParseHex(hex);
}

flowmesh::ProductionEntryCore EntryPrefix(const std::vector<unsigned char>& payload)
{
    if (payload.size() < 4 || payload.size() > MAX_CERTIFIED_BYTES) Invalid("invalid certified payload size");
    const size_t size{ReadBE32(payload.data())};
    if (size == 0 || size > flowmesh::FLOWMESH_V1_MAX_MICROBLOCK_BYTES || size > payload.size() - 4) {
        Invalid("invalid certified entry length");
    }
    const auto entry{flowmesh::DecodeProductionEntry(std::span{payload}.subspan(4, size))};
    if (!entry) Invalid("malformed canonical V1 entry");
    return *entry;
}

bool CanonicalAnchorLocked(ChainstateManager& chainman, const flowmesh::AnchorRef& anchor)
    EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    const auto& chain{chainman.ActiveChain()};
    const auto* block{anchor.height >= 0 ? chain[anchor.height] : nullptr};
    return block && block->GetBlockHash() == anchor.hash &&
        chain.Height() - anchor.height >= Consensus::FLOWMESH_ANCHOR_DEPTH;
}

struct Authority {
    uint256 genesis;
    flowmesh::ClientEvidencePins pins;
    node::FlowMeshMarketRecord market;
    std::optional<node::FlowMeshConnectedCheckpoint> checkpoint;
    flowmesh::ActiveFnBlsSeatSet seats;
    std::vector<flowmesh::BlsSeatBinding> bindings;
};

Authority ResolveAuthorityLocked(ChainstateManager& chainman, const uint256& market_id,
                                 const flowmesh::ProductionEntryCore& entry)
    EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    auto& chainstate{chainman.ActiveChainstate()};
    const auto* tip{chainstate.m_chain.Tip()};
    const auto& params{chainman.GetConsensus()};
    if (!tip || !Consensus::FlowMeshRulesActive(tip->nHeight, params)) Invalid("B3 FlowMesh authority is unavailable");
    const auto domain{params.legacy_final_hash
        ? modern::ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash) : std::nullopt};
    if (!domain) Invalid("B3 chain domain is unavailable");
    auto& seats{chainstate.ModernFnSeats()};
    auto& vaults{chainstate.ModernFlowMeshVaults()};
    auto& checkpoints{chainstate.ModernFlowMeshCheckpoints()};
    // Existing mandatory indexes are rebuildable chain metadata. No FlowMesh
    // engine, microblock replay, signing state, or client journal is touched.
    if (!seats.Sync(chainstate.m_chain, chainstate.m_blockman, params, *tip) ||
        !vaults.Sync(chainstate.m_chain, chainstate.m_blockman, params, *tip) ||
        !checkpoints.Sync(chainstate.m_chain, chainstate.m_blockman, params,
                          seats.Index(), vaults.Index(), *tip)) {
        Invalid("mandatory B3 indexes are unavailable");
    }
    const auto market{vaults.Index().Market(market_id)};
    if (!market) Invalid("market is not established on the local B3 chain");
    flowmesh::ClientEvidencePins pins{*domain, market_id, market->base_asset, market->vault_id,
        flowmesh::ComputeExecutionConfigId(market->vault_id, market->base_asset,
            modern::NativeAsset(), flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS)};
    if (!flowmesh::CheckClientEvidencePins(pins)) Invalid("local B3 market configuration is not canonical");
    const auto checkpoint{checkpoints.Index().Head(market_id)};
    if (checkpoint && (entry.sequence < checkpoint->core.sequence ||
        (entry.sequence == checkpoint->core.sequence && entry.GetHash() != checkpoint->core.microblock_hash))) {
        Invalid("base predates or conflicts with the connected B3 checkpoint");
    }
    // This first bridge deliberately refuses a connected handoff boundary;
    // it does not authorize or implement an incoming-epoch transition.
    if (checkpoint && checkpoint->core.handoff) Invalid("connected handoff is outside this test bootstrap profile");
    std::string error;
    std::vector<flowmesh::BlsSeatBinding> bindings;
    const auto anchored{node::ResolveFlowMeshClientSeats(chainman, pins, entry, error, &bindings)};
    if (!anchored) Invalid(error);
    if (anchored->Size() != SEAT_COUNT || bindings.size() != SEAT_COUNT) Invalid("exactly four real anchored seats are required");
    if (anchored->anchor_height > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) ||
        !CanonicalAnchorLocked(chainman, {static_cast<int32_t>(anchored->anchor_height), anchored->anchor_hash}) ||
        !CanonicalAnchorLocked(chainman, entry.anchor)) Invalid("base or seat anchor is not canonical and deep");
    return {params.hashGenesisBlock, pins, *market, checkpoint, *anchored, std::move(bindings)};
}

bool SameAuthority(const Authority& a, const Authority& b)
{
    return a.genesis == b.genesis && a.pins.domain == b.pins.domain &&
        a.pins.market_id == b.pins.market_id && a.pins.base_asset == b.pins.base_asset &&
        a.pins.vault_id == b.pins.vault_id && a.pins.execution_config_id == b.pins.execution_config_id &&
        a.market == b.market && a.checkpoint == b.checkpoint &&
        a.seats.epoch == b.seats.epoch && a.seats.set_hash == b.seats.set_hash &&
        a.seats.anchor_height == b.seats.anchor_height && a.seats.anchor_hash == b.seats.anchor_hash &&
        std::equal(a.bindings.begin(), a.bindings.end(), b.bindings.begin(), b.bindings.end(),
            [](const auto& x, const auto& y) {
                return x.outpoint == y.outpoint && x.public_key == y.public_key &&
                    x.proof_of_possession == y.proof_of_possession;
            });
}

uint256 Instance(const Authority& authority, const flowmesh::ProductionEntryCore& entry,
                 const uint64_t next_sequence, const uint64_t next_effect_index)
{
    // Fixed TEST/v1 encoding: standard serialization of the listed fixed-width
    // values, then exactly four canonical (seat id, outpoint, key, PoP) tuples.
    // No endpoint metadata, current tip, arrival order, or local key is used.
    HashWriter writer{TaggedHash(INSTANCE_TAG)};
    writer << BOOTSTRAP_VERSION << authority.genesis << authority.pins.domain
           << authority.pins.market_id << authority.pins.base_asset << authority.pins.vault_id
           << authority.pins.execution_config_id << authority.seats.epoch << authority.seats.set_hash
           << authority.seats.anchor_height << authority.seats.anchor_hash << entry.anchor
           << entry.GetHash() << entry.state_root << next_sequence << next_effect_index
           << uint32_t{SEAT_COUNT};
    for (size_t i{0}; i < SEAT_COUNT; ++i) {
        writer << authority.seats.members[i].seat_id << authority.bindings[i].outpoint
               << authority.bindings[i].public_key << authority.bindings[i].proof_of_possession;
    }
    return writer.GetSHA256();
}

UniValue AnchorJSON(const uint64_t height, const uint256& hash)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("height", height);
    out.pushKV("hash", hash.GetHex());
    return out;
}

UniValue VerifyBootstrap(ChainstateManager& chainman, const uint256& market_id,
                         const uint256& expected_head, const flowmesh::ClientStateEvidence& evidence)
{
    const auto entry{EntryPrefix(evidence.certified_payload)};
    if (entry.market_id != market_id || entry.GetHash() != expected_head) Invalid("exact expected market/head does not match the supplied V1 base");
    if (entry.kind != static_cast<uint8_t>(flowmesh::ProductionEntryKind::EXECUTION)) Invalid("handoff entries are outside this test bootstrap profile");
    if (entry.sequence == std::numeric_limits<uint64_t>::max() ||
        entry.effect_count > std::numeric_limits<uint64_t>::max() - entry.effect_start) Invalid("next sequence or effect cursor would overflow");
    const uint64_t next_sequence{entry.sequence + 1};
    const uint64_t next_effect_index{entry.effect_start + entry.effect_count};
    const Authority authority{[&] {
        LOCK(::cs_main);
        return ResolveAuthorityLocked(chainman, market_id, entry);
    }()};
    std::string error;
    const auto verified{flowmesh::VerifyClientStateEvidence(authority.pins, evidence, authority.seats,
        [&](const auto& anchor) {
            LOCK(::cs_main);
            return CanonicalAnchorLocked(chainman, anchor);
        }, error)};
    if (!verified) Invalid(error);
    if (verified->state.ConfigId() != authority.pins.execution_config_id) Invalid("verified state has a different execution configuration");
    {
        LOCK(::cs_main);
        // A normal new block does not invalidate this pinned observation.
        // Re-resolve the actual authority instead of requiring an unchanged tip.
        const auto current{ResolveAuthorityLocked(chainman, market_id, entry)};
        if (!SameAuthority(authority, current)) Invalid("B3 authority changed during verification; retry with the same explicit base pin");
    }
    UniValue out{UniValue::VOBJ};
    out.pushKV("version", BOOTSTRAP_VERSION);
    out.pushKV("profile", BOOTSTRAP_PROFILE);
    out.pushKV("genesis", authority.genesis.GetHex());
    out.pushKV("domain", authority.pins.domain.GetHex());
    out.pushKV("market_id", market_id.GetHex());
    out.pushKV("base_asset_id", authority.pins.base_asset.GetHex());
    out.pushKV("vault_id", authority.pins.vault_id.GetHex());
    out.pushKV("execution_config_id", authority.pins.execution_config_id.GetHex());
    out.pushKV("head", expected_head.GetHex());
    out.pushKV("state_root", verified->state.Root().GetHex());
    out.pushKV("sequence", entry.sequence);
    out.pushKV("next_sequence", next_sequence);
    out.pushKV("effect_start", entry.effect_start);
    out.pushKV("effect_count", entry.effect_count);
    out.pushKV("next_effect_index", next_effect_index);
    out.pushKV("epoch", authority.seats.epoch);
    out.pushKV("seat_set_hash", authority.seats.set_hash.GetHex());
    out.pushKV("entry_anchor", AnchorJSON(static_cast<uint64_t>(entry.anchor.height), entry.anchor.hash));
    out.pushKV("seat_anchor", AnchorJSON(authority.seats.anchor_height, authority.seats.anchor_hash));
    UniValue members{UniValue::VARR};
    for (size_t i{0}; i < SEAT_COUNT; ++i) {
        const auto& binding{authority.bindings[i]};
        UniValue member{UniValue::VOBJ};
        member.pushKV("seat_index", uint64_t{i});
        member.pushKV("seat_id", authority.seats.members[i].seat_id.GetHex());
        member.pushKV("txid", binding.outpoint.hash.GetHex());
        member.pushKV("vout", binding.outpoint.n);
        member.pushKV("public_key", HexStr(binding.public_key));
        member.pushKV("proof_of_possession", HexStr(binding.proof_of_possession));
        members.push_back(std::move(member));
    }
    out.pushKV("bindings", std::move(members));
    out.pushKV("instance", Instance(authority, entry, next_sequence, next_effect_index).GetHex());
    out.pushKV("cutover_authorized", false);
    out.pushKV("execution_authorized", false);
    return out;
}

RPCHelpMan getflowmeshregtestbootstrap()
{
    return RPCHelpMan{
        "getflowmeshregtestbootstrap",
        "TEST ONLY. Export and independently verify the exact expected existing V1-certified head. "
        "The local base pin is test selection, not live cutover authority. No execution, signing, store migration, or settlement.\n",
        {{"market_id", RPCArg::Type::STR, RPCArg::Optional::NO, "Exact nonzero market id"},
         {"expected_head", RPCArg::Type::STR, RPCArg::Optional::NO, "Exact existing V1-certified head hash; no latest-head wildcard"}},
        RPCResult{RPCResult::Type::OBJ, "", "Verified TEST bootstrap context and exact V1 evidence", {}, true},
        RPCExamples{""},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            RequireRegtest();
            const auto market{ExactId(request.params[0], "market_id")};
            const auto head{ExactId(request.params[1], "expected_head")};
            auto& node{EnsureAnyNodeContext(request.context)};
            if (!node.flowmesh) Invalid("export requires an existing local FlowMesh service; use verify on an engine-off client");
            std::string error;
            const auto evidence{node.flowmesh->ClientSnapshot(market, error)};
            if (!evidence) Invalid(error.empty() ? "no existing V1-certified state is available" : error);
            auto out{VerifyBootstrap(EnsureChainman(node), market, head, *evidence)};
            out.pushKV("certified_payload", HexStr(evidence->certified_payload));
            out.pushKV("state_bytes", HexStr(evidence->state_bytes));
            return out;
        }};
}

RPCHelpMan verifyflowmeshregtestbootstrap()
{
    return RPCHelpMan{
        "verifyflowmeshregtestbootstrap",
        "TEST ONLY. Verify a pinned V1-certified base using this node's B3 authority, without a FlowMesh engine. "
        "No endpoint metadata is trusted; no execution, signing, client journal write, or live protocol cutover is authorized.\n",
        {{"market_id", RPCArg::Type::STR, RPCArg::Optional::NO, "Exact nonzero market id"},
         {"expected_head", RPCArg::Type::STR, RPCArg::Optional::NO, "Exact expected V1-certified head hash"},
         {"certified_payload_hex", RPCArg::Type::STR, RPCArg::Optional::NO, "Bounded exact V1 certified payload hex"},
         {"state_hex", RPCArg::Type::STR, RPCArg::Optional::NO, "Bounded canonical state hex, at most 8 MiB decoded"}},
        RPCResult{RPCResult::Type::OBJ, "", "Verified TEST bootstrap context; no protocol authorization", {}, true},
        RPCExamples{""},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            RequireRegtest();
            const auto market{ExactId(request.params[0], "market_id")};
            const auto head{ExactId(request.params[1], "expected_head")};
            flowmesh::ClientStateEvidence evidence{
                BoundedHex(request.params[2], MAX_CERTIFIED_BYTES, "certified_payload_hex"),
                BoundedHex(request.params[3], flowmesh::CLIENT_STATE_MAX_BYTES, "state_hex"), {}};
            return VerifyBootstrap(EnsureChainman(EnsureAnyNodeContext(request.context)), market, head, evidence);
        }};
}

} // namespace

void RegisterFlowMeshP2fvBootstrapRPCCommands(CRPCTable& table)
{
    static const CRPCCommand commands[]{
        {"hidden", &getflowmeshregtestbootstrap},
        {"hidden", &verifyflowmeshregtestbootstrap},
    };
    for (const auto& command : commands) table.appendCommand(command.name, &command);
}
