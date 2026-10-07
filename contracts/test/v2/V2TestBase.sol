// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

/// Foundry cheat-code surface for the bridge V2 suites. Like test/TestBase.sol
/// (which stays untouched for V1), it is declared locally so the suite needs
/// neither forge-std nor a network install. Signatures follow Foundry 1.7.1.
interface VmV2 {
    struct Log {
        bytes32[] topics;
        bytes data;
        address emitter;
    }

    // Environment.
    function warp(uint256 newTimestamp) external;
    function chainId(uint256 newChainId) external;
    function prank(address msgSender) external;
    function prank(address msgSender, address txOrigin) external;
    function startPrank(address msgSender) external;
    function startPrank(address msgSender, address txOrigin) external;
    function stopPrank() external;
    function deal(address account, uint256 newBalance) external;
    function etch(address target, bytes calldata newRuntimeBytecode) external;
    function store(address target, bytes32 slot, bytes32 value) external;
    function load(address target, bytes32 slot) external view returns (bytes32 data);
    function cool(address target) external;
    function label(address account, string calldata newLabel) external;
    function addr(uint256 privateKey) external pure returns (address keyAddr);
    function setEnv(string calldata name, string calldata value) external;
    function snapshotState() external returns (uint256 snapshotId);
    function revertToState(uint256 snapshotId) external returns (bool success);

    // Expectations and logs.
    function expectRevert() external;
    function expectRevert(bytes4 revertData) external;
    function expectRevert(bytes calldata revertData) external;
    function expectEmit() external;
    function expectEmit(address emitter) external;
    function expectEmit(bool checkTopic1, bool checkTopic2, bool checkTopic3, bool checkData) external;
    function expectEmit(bool checkTopic1, bool checkTopic2, bool checkTopic3, bool checkData, address emitter) external;
    function recordLogs() external;
    function getRecordedLogs() external returns (Log[] memory logs);

    // Files and JSON (fs_permissions grants read access to ./test/vectors).
    function readFile(string calldata path) external view returns (string memory data);
    function readFileBinary(string calldata path) external view returns (bytes memory data);
    function keyExistsJson(string calldata json, string calldata key) external view returns (bool);
    function parseJsonUint(string calldata json, string calldata key) external pure returns (uint256);
    function parseJsonUintArray(string calldata json, string calldata key) external pure returns (uint256[] memory);
    function parseJsonBytes32(string calldata json, string calldata key) external pure returns (bytes32);
    function parseJsonBytes32Array(string calldata json, string calldata key) external pure returns (bytes32[] memory);
    function parseJsonBytes(string calldata json, string calldata key) external pure returns (bytes memory);
    function parseJsonAddress(string calldata json, string calldata key) external pure returns (address);
    function parseJsonString(string calldata json, string calldata key) external pure returns (string memory);
    function parseJsonBool(string calldata json, string calldata key) external pure returns (bool);
    function toString(uint256 value) external pure returns (string memory);
    function toString(bytes32 value) external pure returns (string memory);
    function toString(address value) external pure returns (string memory);
    function toString(bytes calldata value) external pure returns (string memory);

    // Assertions (Foundry formats the failing values).
    function assertTrue(bool condition) external pure;
    function assertTrue(bool condition, string calldata err) external pure;
    function assertFalse(bool condition) external pure;
    function assertFalse(bool condition, string calldata err) external pure;
    function assertEq(bool left, bool right) external pure;
    function assertEq(uint256 left, uint256 right) external pure;
    function assertEq(uint256 left, uint256 right, string calldata err) external pure;
    function assertEq(bytes32 left, bytes32 right) external pure;
    function assertEq(bytes32 left, bytes32 right, string calldata err) external pure;
    function assertEq(address left, address right) external pure;
    function assertEq(address left, address right, string calldata err) external pure;
    function assertEq(bytes calldata left, bytes calldata right) external pure;
    function assertEq(bytes calldata left, bytes calldata right, string calldata err) external pure;
    function assertEq(string calldata left, string calldata right) external pure;
    function assertNotEq(uint256 left, uint256 right) external pure;
    function assertNotEq(bytes32 left, bytes32 right) external pure;
    function assertLt(uint256 left, uint256 right) external pure;
    function assertLt(uint256 left, uint256 right, string calldata err) external pure;
    function assertLe(uint256 left, uint256 right) external pure;
    function assertLe(uint256 left, uint256 right, string calldata err) external pure;
    function assertGt(uint256 left, uint256 right) external pure;
    function assertGe(uint256 left, uint256 right) external pure;
}

/// console.log(...) through Foundry's console address; output shows with -vv.
library ConsoleV2 {
    address internal constant CONSOLE = 0x000000000000000000636F6e736F6c652e6c6f67;

    function log(string memory p0) internal view {
        _send(abi.encodeWithSignature("log(string)", p0));
    }

    function log(string memory p0, uint256 p1) internal view {
        _send(abi.encodeWithSignature("log(string,uint256)", p0, p1));
    }

    function log(string memory p0, string memory p1) internal view {
        _send(abi.encodeWithSignature("log(string,string)", p0, p1));
    }

    function log(string memory p0, address p1) internal view {
        _send(abi.encodeWithSignature("log(string,address)", p0, p1));
    }

    function _send(bytes memory payload) private view {
        address console = CONSOLE;
        assembly ("memory-safe") {
            pop(staticcall(gas(), console, add(payload, 0x20), mload(payload), 0, 0))
        }
    }
}

abstract contract V2TestBase {
    VmV2 internal constant vm = VmV2(address(uint160(uint256(keccak256("hevm cheat code")))));

    /// Shared vectors written by test/vectors/gen_v2_vectors.sh (contracts spec §2.5).
    string internal constant V2_VECTORS_PATH = "test/vectors/bridge_v2_vectors.json";

    function readVectorsV2() internal view returns (string memory) {
        return vm.readFile(V2_VECTORS_PATH);
    }

    /// `<prefix>[<index>]<suffix>`, e.g. jsonKey(".lineage", 2, ".lineage_id").
    function jsonKey(string memory prefix, uint256 index, string memory suffix) internal pure returns (string memory) {
        return string.concat(prefix, "[", vm.toString(index), "]", suffix);
    }

    function assertTrue(bool condition) internal pure {
        vm.assertTrue(condition);
    }

    function assertTrue(bool condition, string memory err) internal pure {
        vm.assertTrue(condition, err);
    }

    function assertFalse(bool condition) internal pure {
        vm.assertFalse(condition);
    }

    function assertFalse(bool condition, string memory err) internal pure {
        vm.assertFalse(condition, err);
    }

    function assertEq(bool left, bool right) internal pure {
        vm.assertEq(left, right);
    }

    function assertEq(uint256 left, uint256 right) internal pure {
        vm.assertEq(left, right);
    }

    function assertEq(uint256 left, uint256 right, string memory err) internal pure {
        vm.assertEq(left, right, err);
    }

    function assertEq(bytes32 left, bytes32 right) internal pure {
        vm.assertEq(left, right);
    }

    function assertEq(bytes32 left, bytes32 right, string memory err) internal pure {
        vm.assertEq(left, right, err);
    }

    function assertEq(address left, address right) internal pure {
        vm.assertEq(left, right);
    }

    function assertEq(address left, address right, string memory err) internal pure {
        vm.assertEq(left, right, err);
    }

    function assertEq(bytes memory left, bytes memory right) internal pure {
        vm.assertEq(left, right);
    }

    function assertEq(bytes memory left, bytes memory right, string memory err) internal pure {
        vm.assertEq(left, right, err);
    }

    function assertEq(string memory left, string memory right) internal pure {
        vm.assertEq(left, right);
    }

    function assertNotEq(uint256 left, uint256 right) internal pure {
        vm.assertNotEq(left, right);
    }

    function assertNotEq(bytes32 left, bytes32 right) internal pure {
        vm.assertNotEq(left, right);
    }

    function assertLt(uint256 left, uint256 right) internal pure {
        vm.assertLt(left, right);
    }

    function assertLt(uint256 left, uint256 right, string memory err) internal pure {
        vm.assertLt(left, right, err);
    }

    function assertLe(uint256 left, uint256 right) internal pure {
        vm.assertLe(left, right);
    }

    function assertLe(uint256 left, uint256 right, string memory err) internal pure {
        vm.assertLe(left, right, err);
    }

    function assertGt(uint256 left, uint256 right) internal pure {
        vm.assertGt(left, right);
    }

    function assertGe(uint256 left, uint256 right) internal pure {
        vm.assertGe(left, right);
    }

    receive() external payable {}
}
