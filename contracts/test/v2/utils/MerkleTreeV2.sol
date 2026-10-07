// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

/// Test-only reference for B3's ordered, zero-padded Merkle trees: node =
/// keccak256(left ‖ right), empty subtrees are zero_l with zero_0 = 0 and
/// zero_{l+1} = keccak256(zero_l ‖ zero_l), and bit l of a leaf index orders
/// level l (src/modern/withdrawal_tree.h). One implementation serves the
/// depth-32 checkpoint-history and withdrawal trees and the depth-13 member
/// tree of a SetHeader. Written for clarity, not gas.
library MerkleTreeV2 {
    uint256 internal constant HISTORY_DEPTH = 32;
    uint256 internal constant MEMBER_DEPTH = 13;

    error CountExceedsLeaves();
    error IndexOutsideTree();
    error UnsupportedDepth();

    /// zero_0 .. zero_depth.
    function zeros(uint256 depth) internal pure returns (bytes32[] memory z) {
        if (depth > 64) revert UnsupportedDepth();
        z = new bytes32[](depth + 1);
        for (uint256 level = 0; level < depth; ++level) {
            z[level + 1] = keccak256(abi.encodePacked(z[level], z[level]));
        }
    }

    /// Root over leaves[0 .. count-1]; zero_depth when count is 0.
    function root(bytes32[] memory leaves, uint256 count, uint256 depth) internal pure returns (bytes32) {
        _checkShape(leaves, count, depth);
        bytes32[] memory z = zeros(depth);
        if (count == 0) return z[depth];

        bytes32[] memory nodes = _copy(leaves, count);
        uint256 width = count;
        for (uint256 level = 0; level < depth; ++level) {
            width = _reduce(nodes, width, z[level]);
        }
        return nodes[0];
    }

    /// The `depth` siblings of position `index` in the tree over
    /// leaves[0 .. count-1]. `index` may name an empty position.
    function path(bytes32[] memory leaves, uint256 count, uint256 index, uint256 depth)
        internal
        pure
        returns (bytes32[] memory siblings)
    {
        _checkShape(leaves, count, depth);
        if (index >= (uint256(1) << depth)) revert IndexOutsideTree();
        bytes32[] memory z = zeros(depth);
        siblings = new bytes32[](depth);

        bytes32[] memory nodes = _copy(leaves, count);
        uint256 width = count;
        uint256 position = index;
        for (uint256 level = 0; level < depth; ++level) {
            uint256 sibling = position ^ 1;
            siblings[level] = sibling < width ? nodes[sibling] : z[level];
            width = _reduce(nodes, width, z[level]);
            position >>= 1;
        }
    }

    function path32(bytes32[] memory leaves, uint256 count, uint256 index)
        internal
        pure
        returns (bytes32[32] memory out)
    {
        bytes32[] memory siblings = path(leaves, count, index, HISTORY_DEPTH);
        for (uint256 i = 0; i < HISTORY_DEPTH; ++i) {
            out[i] = siblings[i];
        }
    }

    function path13(bytes32[] memory leaves, uint256 count, uint256 index)
        internal
        pure
        returns (bytes32[13] memory out)
    {
        bytes32[] memory siblings = path(leaves, count, index, MEMBER_DEPTH);
        for (uint256 i = 0; i < MEMBER_DEPTH; ++i) {
            out[i] = siblings[i];
        }
    }

    /// Root implied by `leaf` at `index` and its siblings, any depth.
    function rootFromPath(bytes32 leaf, uint256 index, bytes32[] memory siblings) internal pure returns (bytes32 node) {
        if (siblings.length > 64) revert UnsupportedDepth();
        if (index >= (uint256(1) << siblings.length)) revert IndexOutsideTree();
        node = leaf;
        for (uint256 level = 0; level < siblings.length; ++level) {
            node = ((index >> level) & 1) == 0
                ? keccak256(abi.encodePacked(node, siblings[level]))
                : keccak256(abi.encodePacked(siblings[level], node));
        }
    }

    /// SetHeader member leaf: keccak256(u32 index ‖ 48-byte compressed key ‖ u64 weight).
    function memberLeaf(uint32 index, bytes memory pubkey48, uint64 weight) internal pure returns (bytes32) {
        return keccak256(abi.encodePacked(index, pubkey48, weight));
    }

    function toFixed32(bytes32[] memory siblings) internal pure returns (bytes32[32] memory out) {
        if (siblings.length != HISTORY_DEPTH) revert UnsupportedDepth();
        for (uint256 i = 0; i < HISTORY_DEPTH; ++i) {
            out[i] = siblings[i];
        }
    }

    function toFixed13(bytes32[] memory siblings) internal pure returns (bytes32[13] memory out) {
        if (siblings.length != MEMBER_DEPTH) revert UnsupportedDepth();
        for (uint256 i = 0; i < MEMBER_DEPTH; ++i) {
            out[i] = siblings[i];
        }
    }

    function _checkShape(bytes32[] memory leaves, uint256 count, uint256 depth) private pure {
        if (depth > 64) revert UnsupportedDepth();
        if (count > leaves.length) revert CountExceedsLeaves();
        if (count > (uint256(1) << depth)) revert IndexOutsideTree();
    }

    function _copy(bytes32[] memory leaves, uint256 count) private pure returns (bytes32[] memory nodes) {
        nodes = new bytes32[](count == 0 ? 1 : count);
        for (uint256 i = 0; i < count; ++i) {
            nodes[i] = leaves[i];
        }
    }

    /// Hashes one level in place: nodes[i] = H(nodes[2i], nodes[2i+1] or zero).
    /// Returns the new width. Writing nodes[i] after reading 2i and 2i+1 never
    /// overwrites an unread node.
    function _reduce(bytes32[] memory nodes, uint256 width, bytes32 zero) private pure returns (uint256 next) {
        if (width == 0) return 0;
        next = (width + 1) / 2;
        for (uint256 i = 0; i < next; ++i) {
            bytes32 left = nodes[2 * i];
            bytes32 right = 2 * i + 1 < width ? nodes[2 * i + 1] : zero;
            nodes[i] = keccak256(abi.encodePacked(left, right));
        }
    }
}
