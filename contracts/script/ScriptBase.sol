// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

/// Minimal Foundry cheat-code surface used by the deployment script. This is
/// intentionally local so compiling the contracts never downloads forge-std.
interface ScriptVm {
    function envAddress(string calldata name) external returns (address value);
    function envBytes(string calldata name) external returns (bytes memory value);
    function envBytes32(string calldata name) external returns (bytes32 value);
    function envString(string calldata name) external returns (string memory value);
    function envUint(string calldata name) external returns (uint256 value);
    function envOr(string calldata name, address defaultValue) external returns (address value);
    function startBroadcast() external;
    function startBroadcast(address who) external;
    function stopBroadcast() external;
    function createSelectFork(string calldata urlOrAlias) external returns (uint256 forkId);
    function createSelectFork(string calldata urlOrAlias, uint256 blockNumber) external returns (uint256 forkId);
    function serializeAddress(string calldata objectKey, string calldata valueKey, address value)
        external
        returns (string memory json);
    function serializeBool(string calldata objectKey, string calldata valueKey, bool value)
        external
        returns (string memory json);
    function serializeBytes(string calldata objectKey, string calldata valueKey, bytes calldata value)
        external
        returns (string memory json);
    function serializeBytes32(string calldata objectKey, string calldata valueKey, bytes32 value)
        external
        returns (string memory json);
    function serializeString(string calldata objectKey, string calldata valueKey, string calldata value)
        external
        returns (string memory json);
    function serializeUint(string calldata objectKey, string calldata valueKey, uint256 value)
        external
        returns (string memory json);
    function writeJson(string calldata json, string calldata path) external;
}

abstract contract ScriptBase {
    ScriptVm internal constant vm = ScriptVm(address(uint160(uint256(keccak256("hevm cheat code")))));
}
