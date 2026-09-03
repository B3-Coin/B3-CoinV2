// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {ProductionBridgeScript} from "./ProductionBridgeScript.sol";
import {ProductionBridgeConfig} from "./ProductionBridgeConfig.sol";

/// Post-broadcast verifier and final-manifest writer.
///
/// This proves that the configured vault address had no code immediately
/// before ORIGIN_DEPLOYMENT_BLOCK and had the exact reviewed runtime/config at
/// that block. It then repeats the runtime/config check at the RPC's latest
/// block. Every config/artifact field must also reproduce the independently
/// approved Deploy candidate commitment. Only this script emits
/// `deployment_inputs_complete: true`; it never emits production approval.
contract FinalizeDeployment is ProductionBridgeScript {
    error BadDeploymentBlock();
    error VaultExistedBeforeDeclaredBlock();
    error VaultMissingAtDeclaredBlock();
    error DeploymentNotDeepEnough(uint256 actual, uint256 required);

    function run() external {
        ProductionBridgeConfig.Config memory config = _readConfig();
        ProductionBridgeConfig.Artifacts memory artifacts = _readArtifacts();
        bytes32 expectedDeploymentConfigHash = vm.envBytes32("EXPECTED_DEPLOYMENT_CONFIG_HASH");
        ProductionBridgeConfig.validateDeploymentConfigHash(config, artifacts, expectedDeploymentConfigHash);
        string memory rpcUrl = vm.envString("ETH_RPC_URL");
        string memory manifestPath = vm.envString("FINAL_MANIFEST_PATH");
        uint256 deploymentBlock = vm.envUint("ORIGIN_DEPLOYMENT_BLOCK");
        uint256 requiredConfirmations = vm.envUint("DEPLOYMENT_CONFIRMATIONS_REQUIRED");
        if (deploymentBlock == 0 || deploymentBlock > type(uint64).max || requiredConfirmations == 0) {
            revert BadDeploymentBlock();
        }

        vm.createSelectFork(rpcUrl, deploymentBlock - 1);
        if (block.chainid != config.expectedChainId) {
            revert ProductionBridgeConfig.WrongChain(config.expectedChainId, block.chainid);
        }
        if (artifacts.bridge.code.length != 0) {
            revert VaultExistedBeforeDeclaredBlock();
        }

        vm.createSelectFork(rpcUrl, deploymentBlock);
        if (artifacts.bridge.code.length == 0) {
            revert VaultMissingAtDeclaredBlock();
        }
        _validateConfig(config);
        _verifyStack(config, artifacts);

        vm.createSelectFork(rpcUrl);
        if (block.number < deploymentBlock) revert BadDeploymentBlock();
        if (block.number - deploymentBlock < requiredConfirmations) {
            revert DeploymentNotDeepEnough(block.number - deploymentBlock, requiredConfirmations);
        }
        if (block.chainid != config.expectedChainId) {
            revert ProductionBridgeConfig.WrongChain(config.expectedChainId, block.chainid);
        }
        // Revalidate every time-sensitive input against the latest block as
        // well as the historical deployment block. Otherwise an operator
        // could finalize after BOOTSTRAP_DEADLINE, publish a seemingly
        // complete manifest, and accept deposits into a verifier that can no
        // longer perform its one-time Set0 handoff.
        _validateConfig(config);
        _verifyStack(config, artifacts);
        _writeManifest(manifestPath, config, artifacts, true, deploymentBlock);
    }
}
