// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

/// Byte-for-byte mirror of modern::BridgeAssetIdV1 on B3.
///
/// B3 serializes uint64 values little-endian. Digest and address byte arrays
/// retain wire/EVM order. Keep the cross-language fixture in
/// test/BridgeAssetIdVector.t.sol and src/test/bridge_asset_vector_tests.cpp in
/// lockstep with this function.
library B3BridgeAssetId {
    function compute(bytes32 chainDomain, uint64 originChainId, address vault, address token)
        internal
        pure
        returns (bytes32)
    {
        bytes32 tagHash = sha256("B3/BRIDGE/ASSET/V1");
        return sha256(
            abi.encodePacked(
                tagHash,
                tagHash,
                chainDomain,
                uint8(1),
                _littleEndian64(originChainId),
                vault,
                token,
                uint8(6),
                uint8(6)
            )
        );
    }

    function _littleEndian64(uint64 value) private pure returns (bytes memory out) {
        out = new bytes(8);
        for (uint256 i = 0; i < 8; ++i) {
            out[i] = bytes1(uint8(value >> (i * 8)));
        }
    }
}
