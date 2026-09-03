// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {ScriptBase} from "./ScriptBase.sol";
import {B3DepositVault} from "../B3DepositVault.sol";

/// Historical managed-v1 deployment helper. It is intentionally not the
/// default Deploy script and must not be mistaken for the decentralized stack.
contract DeployManagedVault is ScriptBase {
    function run() external {
        address authority = vm.envAddress("RELEASE_AUTHORITY");
        address rescuer = vm.envOr("RESCUE_AUTHORITY", authority);
        require(authority != address(0) && rescuer != address(0), "ZERO_AUTHORITY");
        vm.startBroadcast();
        new B3DepositVault(authority, rescuer);
        vm.stopBroadcast();
    }
}
