// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

/// Minimal Foundry cheat-code surface used by the deployment script. This is
/// intentionally local so compiling the contracts never downloads forge-std.
interface ScriptVm {
    function envAddress(string calldata name) external returns (address value);
    function envOr(string calldata name, address defaultValue)
        external
        returns (address value);
    function startBroadcast() external;
    function stopBroadcast() external;
}

abstract contract ScriptBase {
    ScriptVm internal constant vm =
        ScriptVm(address(uint160(uint256(keccak256("hevm cheat code")))));
}
