// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {B3Types} from "../IB3FinalityProver.sol";

/// Shared, fixed-width objects and hashes for the B3 bridge V2 contracts.
///
/// doc/design/b3-bridge-v2-contracts-spec.md §2.1-§2.2, §2.5 and §6.4 are
/// normative for every byte layout here. Integers are big-endian
/// (abi.encodePacked) except the little-endian origin chain id inside the
/// AssetId preimage, which mirrors B3's BridgeAssetIdV1 serialization
/// (src/modern/bridge_asset.h). V2 reuses V1's 110-byte SetHeader
/// (B3Types.SetHeader) with rulesetVersion 2, so every V1 header is rejected
/// by headerShapeValid below. The V1 library is imported, never modified.
///
/// Tag hashes are hard-coded so no sha256 precompile call is spent on them;
/// test/v2/VectorsV2.t.sol checks each one against its string.
library B3TypesV2 {
    uint16 internal constant RULESET_V2 = 2;
    /// Depth of the checkpoint history and withdrawal trees (spec §2.2, §6.4).
    uint256 internal constant HISTORY_DEPTH = 32;
    /// Exclusive bound for checkpoint indices and withdrawal ids (depth-32 trees).
    uint256 internal constant MAX_INDEX = 1 << HISTORY_DEPTH;

    /// Packed lengths of the hashed objects.
    uint256 internal constant FINALIZED_BLOCK_V2_LENGTH = 185;
    uint256 internal constant ASSET_ID_PREIMAGE_LENGTH = 147;
    uint256 internal constant WITHDRAWAL_LEAF_PREIMAGE_LENGTH = 180;

    /// sha256("B3/FINALITY/V2")
    bytes32 internal constant FINALITY_TAG = 0x0c34706d64f8869715ef48e23985b870335fe1f2afd7757587a890447d81ab73;
    /// sha256("B3/FINALITY/LINEAGE/V2")
    bytes32 internal constant LINEAGE_TAG = 0x0e9e849914361999bf1b954c1a2ec3545fbe6d113f93b8ca21d0a45b56d8f9d3;
    /// sha256("B3/BRIDGE/ASSET/V2")
    bytes32 internal constant ASSET_TAG = 0x7bf39d27dc845091cf66adcf93912e3ffeaed119a38c3d5bf4479821a90e16ce;
    /// keccak256("B3/BRIDGE/WITHDRAWAL/V2")
    bytes32 internal constant LEAF_TAG_V2 = 0xc250a4de311e4369a5d75a6fdd6351f1cc8ec35929170d1374a5367a5edd10f5;

    /// AssetId serialization version byte (BridgeAssetIdV1 layout, version 2).
    uint8 internal constant ASSET_ID_VERSION = 2;
    /// USDT origin and bUSD V2 asset decimals.
    uint8 internal constant ASSET_DECIMALS = 6;

    /// Verifier status values (spec §3.2). B3 and callers test status == LIVE;
    /// "initialized and not halted" would read CANCELLED as live.
    uint8 internal constant STATUS_UNINITIALIZED = 0;
    uint8 internal constant STATUS_LIVE = 1;
    uint8 internal constant STATUS_HALTED_CONFLICT = 2;
    uint8 internal constant STATUS_CANCELLED = 3;

    /// bridgeFlags bit 0: B3 closed burns for good; the verifier closes deposits for good.
    uint8 internal constant FLAG_DEPOSITS_CLOSED = 0x01;

    /// rootFromPath was given an index outside the depth-32 tree.
    error IndexOutOfRange();

    /// 185 bytes packed, big-endian, in this field order (spec §2.1).
    struct FinalizedBlockV2 {
        bytes32 lineageId;
        uint64 height;
        bytes32 blockHash;
        bytes32 withdrawalRoot; // zero below BRIDGE_ACTIVATION_HEIGHT (V), the V2 tree root from V
        bytes32 validatorSetHash; // setHash of the successor SetHeader
        uint64 epoch; // epoch of the signing set
        uint64 checkpointIndex; // >= 1; position of this checkpoint in the history tree
        bytes32 checkpointHistoryRoot; // depth-32 root over leaves 0 .. checkpointIndex-1
        uint8 bridgeFlags; // bit 0: B3 closed burns for good; other bits zero
    }

    /// One B3 burn as the vault releases it (spec §6.4). Declared here, not in
    /// the vault, so the relay batch and tests share it without importing the vault.
    struct WithdrawalV2 {
        uint64 withdrawalId;
        address recipient;
        uint256 amount;
        uint64 b3Height;
    }

    /// Packed 185-byte statement: [0,32) lineageId, [32,40) height,
    /// [40,72) blockHash, [72,104) withdrawalRoot, [104,136) validatorSetHash,
    /// [136,144) epoch, [144,152) checkpointIndex, [152,184)
    /// checkpointHistoryRoot, [184] bridgeFlags.
    function encode(FinalizedBlockV2 memory fb) internal pure returns (bytes memory) {
        return abi.encodePacked(
            fb.lineageId,
            fb.height,
            fb.blockHash,
            fb.withdrawalRoot,
            fb.validatorSetHash,
            fb.epoch,
            fb.checkpointIndex,
            fb.checkpointHistoryRoot,
            fb.bridgeFlags
        );
    }

    /// digestV2 = TaggedHash("B3/FINALITY/V2", chainDomain ‖ encode(fb)); the BLS message.
    function digest(bytes32 chainDomain, FinalizedBlockV2 memory fb) internal pure returns (bytes32) {
        return sha256(abi.encodePacked(FINALITY_TAG, FINALITY_TAG, chainDomain, encode(fb)));
    }

    /// certId = keccak256(encode(fb)); equal for every aggregation of one statement.
    function certId(FinalizedBlockV2 memory fb) internal pure returns (bytes32) {
        return keccak256(encode(fb));
    }

    /// lineageId = TaggedHash("B3/FINALITY/LINEAGE/V2",
    ///   chainDomain ‖ u64 genesisHeight ‖ genesisBlockHash ‖ genesisSetHash).
    function lineageId(bytes32 chainDomain, uint64 genesisHeight, bytes32 genesisBlockHash, bytes32 genesisSetHash)
        internal
        pure
        returns (bytes32)
    {
        return sha256(
            abi.encodePacked(LINEAGE_TAG, LINEAGE_TAG, chainDomain, genesisHeight, genesisBlockHash, genesisSetHash)
        );
    }

    /// historyLeaf(h, hash) = keccak256(u64 h ‖ hash), 40 bytes, keyed by height.
    function historyLeaf(uint64 height, bytes32 blockHash) internal pure returns (bytes32 leaf) {
        assembly ("memory-safe") {
            mstore(0x00, shl(192, height))
            mstore(0x08, blockHash)
            leaf := keccak256(0x00, 0x28)
        }
    }

    /// Root of the depth-32 ordered tree containing `leaf` at `index`. Bit l of
    /// the index orders level l: 0 hashes node ‖ sibling, 1 hashes sibling ‖ node
    /// (as contracts/src/B3StakerBridge.sol and src/modern/withdrawal_tree.h).
    /// The path supplies every sibling, zero subtrees included, so no zero table
    /// is needed on-chain. Reverts IndexOutOfRange unless index < 2^32, so two
    /// indices can never name the same position.
    function rootFromPath(bytes32 leaf, uint256 index, bytes32[HISTORY_DEPTH] memory path)
        internal
        pure
        returns (bytes32 node)
    {
        if (index >= MAX_INDEX) revert IndexOutOfRange();
        assembly ("memory-safe") {
            node := leaf
            for { let level := 0 } lt(level, HISTORY_DEPTH) { level := add(level, 1) } {
                let sibling := mload(add(path, shl(5, level)))
                switch and(shr(level, index), 1)
                case 0 {
                    mstore(0x00, node)
                    mstore(0x20, sibling)
                }
                default {
                    mstore(0x00, sibling)
                    mstore(0x20, node)
                }
                node := keccak256(0x00, 0x40)
            }
        }
    }

    /// V1's structural SetHeader rule (B3Types.headerShapeValid) on ruleset 2:
    /// count in 1..8192, totalWeight > 0, quorumWeight == floor(2W/3)+1, a
    /// 48-byte compressed G1 aggregate (compression bit set, infinity bit clear)
    /// and a non-zero members root. Point validity is left to the BLS precompiles.
    function headerShapeValid(B3Types.SetHeader memory header) internal pure returns (bool) {
        if (
            header.rulesetVersion != RULESET_V2 || header.validatorCount == 0
                || header.validatorCount > B3Types.MAX_VALIDATORS || header.totalWeight == 0
                || uint256(header.quorumWeight) != B3Types.quorumWeight(header.totalWeight)
                || header.aggregatePubkey.length != 48 || header.membersRoot == bytes32(0)
        ) return false;

        uint8 flags = uint8(header.aggregatePubkey[0]) & 0xc0;
        return flags == 0x80;
    }

    /// Bridge qualification of a set: min <= count <= max and weight >= minWeight.
    function qualified(uint256 count, uint256 weight, uint256 minCount, uint256 maxCount, uint256 minWeight)
        internal
        pure
        returns (bool)
    {
        return count >= minCount && count <= maxCount && weight >= minWeight;
    }

    /// bUSD V2 AssetId = TaggedHash("B3/BRIDGE/ASSET/V2", chainDomain ‖ 0x02 ‖
    /// LE64(originChainId) ‖ vault ‖ token ‖ 0x06 ‖ 0x06): the 147-byte
    /// BridgeAssetIdV1 serialization with version byte 2 (node changes §3.2).
    function assetId(bytes32 chainDomain, uint64 originChainId, address vault, address token)
        internal
        pure
        returns (bytes32)
    {
        return sha256(
            abi.encodePacked(
                ASSET_TAG,
                ASSET_TAG,
                chainDomain,
                ASSET_ID_VERSION,
                _littleEndian64(originChainId),
                vault,
                token,
                ASSET_DECIMALS,
                ASSET_DECIMALS
            )
        );
    }

    /// Leaf V2 = keccak256 of the 180-byte preimage LEAF_TAG_V2 ‖ assetId ‖
    /// u64 id ‖ u64 chainId ‖ vault ‖ token ‖ recipient ‖ u256 amount ‖
    /// u64 b3Height (spec §6.4). A migration leaf is an ordinary leaf.
    function withdrawalLeaf(
        bytes32 b3AssetId,
        uint64 withdrawalId,
        uint64 chainId,
        address vault,
        address token,
        address recipient,
        uint256 amount,
        uint64 b3Height
    ) internal pure returns (bytes32) {
        return keccak256(
            abi.encodePacked(LEAF_TAG_V2, b3AssetId, withdrawalId, chainId, vault, token, recipient, amount, b3Height)
        );
    }

    function _littleEndian64(uint64 value) private pure returns (bytes8) {
        value = ((value & 0xff00ff00ff00ff00) >> 8) | ((value & 0x00ff00ff00ff00ff) << 8);
        value = ((value & 0xffff0000ffff0000) >> 16) | ((value & 0x0000ffff0000ffff) << 16);
        value = (value >> 32) | (value << 32);
        return bytes8(value);
    }
}
