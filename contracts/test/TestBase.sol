// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

/// Minimal Foundry cheat-code surface used by this repository's tests.
/// Keeping it local makes the test suite reproducible without forge-std or a
/// network install.
interface Vm {
    function expectRevert(bytes4 revertData) external;
    function expectEmit(bool checkTopic1, bool checkTopic2, bool checkTopic3, bool checkData, address emitter) external;
    function prank(address sender) external;
    function deal(address account, uint256 newBalance) external;
    function readFileBinary(string calldata path) external view returns (bytes memory data);
    function warp(uint256 newTimestamp) external;
}

abstract contract TestBase {
    Vm internal constant vm = Vm(address(uint160(uint256(keccak256("hevm cheat code")))));

    function assertEq(uint256 left, uint256 right) internal pure {
        require(left == right, "assertEq(uint256)");
    }

    function assertEq(bytes32 left, bytes32 right) internal pure {
        require(left == right, "assertEq(bytes32)");
    }

    function assertTrue(bool value) internal pure {
        require(value, "assertTrue");
    }

    function assertFalse(bool value) internal pure {
        require(!value, "assertFalse");
    }

    receive() external payable {}
}
