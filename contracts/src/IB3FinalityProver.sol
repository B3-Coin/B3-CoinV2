// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

/// Shared, fixed-width objects for the B3 staker-finality bridge.
///
/// Every integer is encoded big-endian by abi.encodePacked. The resulting
/// byte layouts are the 112-byte FinalizedBlock and 110-byte SetHeader from
/// doc/design/b3-cross-chain-finality-v1.md. No private validator material is
/// embedded here. A deployment may pin a four-key bootstrap header first and
/// install the canonical Set_0 header only after B3 derives it from public
/// corridor state.
library B3Types {
    uint16 internal constant RULESET_V1 = 1;
    uint32 internal constant MAX_VALIDATORS = 8_192;
    uint256 internal constant MIN_PROVEN_EPOCH_DURATION = 1 days;
    // Largest bridge-authorizing set exercised by the V1 target-fork gas
    // benchmark. Larger B3 sets remain valid chain-lineage objects, but they
    // deliberately cannot authorize new bridge roots in this deployment.
    uint32 internal constant MAX_PROVEN_BRIDGE_VALIDATORS = 64;

    struct FinalizedBlock {
        uint64 height;
        bytes32 blockHash;
        bytes32 withdrawalRoot;
        bytes32 validatorSetHash;
        uint64 epoch;
    }

    struct SetHeader {
        uint64 epoch;
        uint16 rulesetVersion;
        uint32 validatorCount;
        uint64 totalWeight;
        uint64 quorumWeight;
        bytes aggregatePubkey; // canonical 48-byte compressed BLS12-381 G1
        bytes32 membersRoot;
    }

    function encodeSetHeader(SetHeader memory header) internal pure returns (bytes memory) {
        return abi.encodePacked(
            header.epoch,
            header.rulesetVersion,
            header.validatorCount,
            header.totalWeight,
            header.quorumWeight,
            header.aggregatePubkey,
            header.membersRoot
        );
    }

    function hashSetHeader(SetHeader memory header) internal pure returns (bytes32) {
        return keccak256(encodeSetHeader(header));
    }

    function quorumWeight(uint64 totalWeight) internal pure returns (uint256) {
        return (uint256(totalWeight) * 2) / 3 + 1;
    }

    /// Structural rules that Ethereum can verify without knowing the member
    /// list. Point validity is enforced by the BLS precompiles when a
    /// certificate is submitted.
    function headerShapeValid(SetHeader memory header) internal pure returns (bool) {
        if (
            header.rulesetVersion != RULESET_V1 || header.validatorCount == 0 || header.validatorCount > MAX_VALIDATORS
                || header.totalWeight == 0 || uint256(header.quorumWeight) != quorumWeight(header.totalWeight)
                || header.aggregatePubkey.length != 48 || header.membersRoot == bytes32(0)
        ) return false;

        // IETF compressed G1: compression bit set, infinity bit clear.
        uint8 flags = uint8(header.aggregatePubkey[0]) & 0xc0;
        return flags == 0x80;
    }
}

/// Immutable proof-backend seam. V1 is the EIP-2537 BLS prover; a separately
/// deployed verifier would be required to select another backend.
interface IB3FinalityProver {
    function verify(
        bytes32 chainDomain,
        B3Types.FinalizedBlock calldata finalizedBlock,
        bytes32 signingSetHash,
        B3Types.SetHeader calldata signingSet,
        bytes calldata proof
    ) external view returns (bool ok);
}
