// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {ScriptBase} from "./ScriptBase.sol";
import {B3DepositVault} from "../B3DepositVault.sol";

/// Deploys B3DepositVault. RELEASE_AUTHORITY must be set in the environment.
/// For interim/testing deployments that address is an owner-controlled
/// wallet (funds recoverable by the owner, trust-labeled); mainnet
/// production waits for the section-5 finality verifier stack.
contract Deploy is ScriptBase {
    function run() external {
        address authority = vm.envAddress("RELEASE_AUTHORITY");
        address rescuer = vm.envOr("RESCUE_AUTHORITY", authority);
        vm.startBroadcast();
        new B3DepositVault(authority, rescuer);
        vm.stopBroadcast();
    }
}
