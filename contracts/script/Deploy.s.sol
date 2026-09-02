// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {ProductionBridgeScript} from "./ProductionBridgeScript.sol";
import {ProductionBridgeConfig} from "./ProductionBridgeConfig.sol";
import {B3Types, IB3FinalityProver} from "../src/IB3FinalityProver.sol";
import {BlsCertificateProver} from "../src/BlsCertificateProver.sol";
import {B3FinalityVerifier} from "../src/B3FinalityVerifier.sol";
import {B3StakerBridge} from "../src/B3StakerBridge.sol";

/// Deploy the immutable decentralized bridge stack in dependency order:
/// prover -> finality verifier -> single-token vault.
///
/// Every constructor/security value is mandatory in the environment. The
/// resulting JSON is deliberately a candidate: an Ethereum deployment block
/// cannot be known during Foundry's pre-broadcast simulation. Run
/// FinalizeDeployment.s.sol against the mined chain with the independently
/// approved candidate commitment before copying any value into B3 chainparams.
contract Deploy is ProductionBridgeScript {
    function run() external {
        ProductionBridgeConfig.Config memory config = _readConfig();
        _validateConfig(config);
        string memory manifestPath = vm.envString("CANDIDATE_MANIFEST_PATH");

        vm.startBroadcast(config.deployer);

        BlsCertificateProver prover = new BlsCertificateProver();
        bytes32 proverCodeHash = _codeHash(address(prover));

        B3Types.SetHeader memory bootstrap = ProductionBridgeConfig.bootstrapSet(config);
        B3FinalityVerifier verifier = new B3FinalityVerifier(
            config.chainDomain,
            bootstrap,
            config.expectedBootstrapSetHash,
            IB3FinalityProver(address(prover)),
            proverCodeHash,
            ProductionBridgeConfig.verifierConfig(config)
        );
        bytes32 verifierCodeHash = _codeHash(address(verifier));

        B3StakerBridge bridge = new B3StakerBridge(
            verifier, verifierCodeHash, config.chainDomain, config.originToken, config.maxDepositRaw
        );
        bytes32 bridgeCodeHash = _codeHash(address(bridge));

        vm.stopBroadcast();

        ProductionBridgeConfig.Artifacts memory artifacts = ProductionBridgeConfig.Artifacts({
            prover: address(prover),
            proverCodeHash: proverCodeHash,
            verifier: address(verifier),
            verifierCodeHash: verifierCodeHash,
            bridge: address(bridge),
            bridgeCodeHash: bridgeCodeHash,
            bridgeAssetId: bridge.B3_ASSET_ID()
        });
        _verifyStack(config, artifacts);
        _writeManifest(manifestPath, config, artifacts, false, 0);
    }
}
