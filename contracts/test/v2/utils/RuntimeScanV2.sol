// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

/// Opcode scan of a deployed runtime (test only). It walks the executable
/// part of the code, before the CBOR metadata, and skips PUSH1..PUSH32 data,
/// so constants and metadata never count as opcodes. It stands in for spec
/// §12's check-no-number-opcode.sh (not created in stage S3): every V2
/// runtime must be free of NUMBER so it can later run on Arbitrum, where
/// block.number is the L1 estimate.
library RuntimeScanV2 {
    uint8 internal constant NUMBER = 0x43;
    uint8 internal constant SLOAD = 0x54;
    uint8 internal constant SSTORE = 0x55;
    uint8 internal constant TLOAD = 0x5c;
    uint8 internal constant TSTORE = 0x5d;
    uint8 internal constant CREATE = 0xf0;
    uint8 internal constant CALLCODE = 0xf2;
    uint8 internal constant DELEGATECALL = 0xf4;
    uint8 internal constant CREATE2 = 0xf5;
    uint8 internal constant SELFDESTRUCT = 0xff;

    /// Bytes before the CBOR metadata (whose length is the last two bytes).
    function executableEnd(bytes memory code) internal pure returns (uint256) {
        uint256 n = code.length;
        require(n > 2, "code");
        uint256 cborLength = (uint256(uint8(code[n - 2])) << 8) | uint256(uint8(code[n - 1]));
        require(cborLength + 2 < n, "metadata");
        return n - 2 - cborLength;
    }

    /// How often each opcode occurs in the executable code, and the total.
    function opcodeCounts(bytes memory code) internal pure returns (uint256[256] memory counts, uint256 total) {
        uint256 end = executableEnd(code);
        for (uint256 pc = 0; pc < end; ++pc) {
            uint8 op = uint8(code[pc]);
            ++counts[op];
            ++total;
            if (op >= 0x60 && op <= 0x7f) pc += op - 0x5f; // skip PUSH1..PUSH32 data
        }
    }
}
