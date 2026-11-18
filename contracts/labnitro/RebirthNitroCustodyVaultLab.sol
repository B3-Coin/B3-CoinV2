// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {RebirthCustodyVault} from "../custody/RebirthCustodyVault.sol";

/// Owned Nitro development chain only. This is not an Arbitrum One deployment,
/// proof verifier, settlement claim, permissionless exit, or production release.
/// All accounting, authorization and withdrawal logic is inherited unchanged.
/// In particular the non-31337 five-minute/7-day policy floors still apply.
contract RebirthNitroCustodyVaultLab is RebirthCustodyVault {
    uint256 public constant LAB_CHAIN_ID = 412346;
    bool public constant LAB_ONLY = true;
    error NitroLabChainRequired();

    constructor(address token, bytes32 qAssetId, bytes32 instanceId, address[4] memory roster, Policy memory policy)
        RebirthCustodyVault(token, qAssetId, instanceId, roster, policy)
    {
        if (block.chainid != LAB_CHAIN_ID || policy.chainId != LAB_CHAIN_ID) revert NitroLabChainRequired();
    }
}
