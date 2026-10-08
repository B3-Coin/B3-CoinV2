// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {VmV2} from "../V2TestBase.sol";

/// Worst-case size of the receipt-trie leaf B3 parses for a deposit or flush
/// (test only; contracts spec §7.1, PR-1 and PR-3; node changes §4).
///
/// It RLP-encodes, byte for byte, the leaf node of an EIP-2718 type-2 receipt
/// carrying the recorded logs:
///
///   receipt = 0x02 ‖ rlp([status, cumulativeGasUsed, logsBloom, logs])
///   log     = rlp([address, [topic...], data])
///   leaf    = rlp([hexPrefix(path), receipt])
///
/// Every field whose size the transaction cannot choose takes its worst case:
/// status 1, cumulativeGasUsed with 8 significant bytes (above any block gas
/// limit), the 256-byte bloom (its content never changes its size), and the
/// longest leaf path: all 6 nibbles of a 3-byte key rlp(tx_index) (indexes
/// 256..65,535), hex-prefix encoded with the leaf flag. Logs are encoded
/// exactly as recorded, so the result is the size a relayer predicts before
/// it flushes (PR-3 skips a flush above SKIP_LIMIT).
library ReceiptSizeV2 {
    /// EIP-1559 transaction type, the receipt's type byte.
    uint8 internal constant TX_TYPE = 0x02;
    /// Bytes of logsBloom.
    uint256 internal constant BLOOM_BYTES = 256;
    /// A cumulativeGasUsed with 8 significant bytes.
    uint256 internal constant WORST_CUMULATIVE_GAS = type(uint64).max;
    /// rlp(0xffff): the longest key a receipt of a real block has.
    bytes internal constant WORST_KEY = hex"82ffff";

    /// PR-3: relayers skip a flush whose predicted leaf exceeds this.
    uint256 internal constant SKIP_LIMIT = 7_168;
    /// Largest leaf in the cases contracts spec §7.1 models.
    uint256 internal constant MODELLED_MAX = 1_133;
    /// The V2 store's terminal receipt-node limit (src/bridge/proof.h).
    uint256 internal constant TERMINAL_NODE_LIMIT = 8_192;

    /// Size of the worst-case leaf node for a receipt with `logs`.
    function leafSize(VmV2.Log[] memory logs) internal pure returns (uint256) {
        return leafNode(logs).length;
    }

    /// The worst-case leaf node: rlp([hexPrefix(WORST_KEY nibbles), receipt]).
    function leafNode(VmV2.Log[] memory logs) internal pure returns (bytes memory) {
        // Hex-prefix of an even-length leaf path: flag nibble 2, pad nibble 0.
        bytes memory path = abi.encodePacked(bytes1(0x20), WORST_KEY);
        return encodeList(abi.encodePacked(encodeBytes(path), encodeBytes(typedReceipt(logs))));
    }

    /// 0x02 ‖ rlp([1, WORST_CUMULATIVE_GAS, bloom, logs]).
    function typedReceipt(VmV2.Log[] memory logs) internal pure returns (bytes memory) {
        bytes memory encodedLogs;
        for (uint256 i = 0; i < logs.length; ++i) {
            encodedLogs = abi.encodePacked(encodedLogs, encodeLog(logs[i]));
        }
        bytes memory body = abi.encodePacked(
            encodeUint(1),
            encodeUint(WORST_CUMULATIVE_GAS),
            encodeBytes(new bytes(BLOOM_BYTES)),
            encodeList(encodedLogs)
        );
        return abi.encodePacked(bytes1(TX_TYPE), encodeList(body));
    }

    /// rlp([address, [topic...], data]).
    function encodeLog(VmV2.Log memory log) internal pure returns (bytes memory) {
        bytes memory topics;
        for (uint256 i = 0; i < log.topics.length; ++i) {
            topics = abi.encodePacked(topics, encodeBytes(abi.encodePacked(log.topics[i])));
        }
        return encodeList(
            abi.encodePacked(encodeBytes(abi.encodePacked(log.emitter)), encodeList(topics), encodeBytes(log.data))
        );
    }

    // ===================================================================== RLP

    function encodeBytes(bytes memory value) internal pure returns (bytes memory) {
        if (value.length == 1 && uint8(value[0]) < 0x80) return value;
        return abi.encodePacked(_prefix(0x80, value.length), value);
    }

    function encodeList(bytes memory payload) internal pure returns (bytes memory) {
        return abi.encodePacked(_prefix(0xc0, payload.length), payload);
    }

    /// Minimal big-endian bytes of `value` as an RLP string (0 is 0x80).
    function encodeUint(uint256 value) internal pure returns (bytes memory) {
        return encodeBytes(_minimalBigEndian(value));
    }

    function _prefix(uint8 offset, uint256 length) private pure returns (bytes memory) {
        if (length <= 55) {
            // forge-lint: disable-next-line(unsafe-typecast)
            return abi.encodePacked(bytes1(uint8(offset + length)));
        }
        bytes memory lengthBytes = _minimalBigEndian(length);
        // forge-lint: disable-next-line(unsafe-typecast)
        return abi.encodePacked(bytes1(uint8(offset + 55 + lengthBytes.length)), lengthBytes);
    }

    function _minimalBigEndian(uint256 value) private pure returns (bytes memory out) {
        uint256 n = 0;
        for (uint256 v = value; v != 0; v >>= 8) {
            ++n;
        }
        out = new bytes(n);
        for (uint256 i = 0; i < n; ++i) {
            // forge-lint: disable-next-line(unsafe-typecast)
            out[n - 1 - i] = bytes1(uint8(value >> (8 * i)));
        }
    }
}
