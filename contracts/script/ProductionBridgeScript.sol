// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {ScriptBase} from "./ScriptBase.sol";
import {ProductionBridgeConfig} from "./ProductionBridgeConfig.sol";
import {B3Types} from "../src/IB3FinalityProver.sol";
import {BlsCertificateProver} from "../src/BlsCertificateProver.sol";
import {B3FinalityVerifier} from "../src/B3FinalityVerifier.sol";
import {B3StakerBridge} from "../src/B3StakerBridge.sol";

abstract contract ProductionBridgeScript is ScriptBase {
    error IntegerOutOfRange(string variableName);
    error TokenMetadataUnavailable();
    error OnchainConfigurationMismatch();

    function _readConfig() internal returns (ProductionBridgeConfig.Config memory config) {
        config.deployer = vm.envAddress("EXPECTED_DEPLOYER");
        config.expectedChainId = vm.envUint("EXPECTED_CHAIN_ID");
        config.chainDomain = vm.envBytes32("B3_CHAIN_DOMAIN");
        config.originToken = vm.envAddress("ORIGIN_TOKEN");
        config.expectedOriginTokenCodeHash = vm.envBytes32("EXPECTED_ORIGIN_TOKEN_CODE_HASH");
        config.maxDepositRaw = _envUint64("MAX_DEPOSIT_RAW");
        config.b3MaxPerBlockRaw = _envUint64("B3_MAX_PER_BLOCK_RAW");
        config.bootstrapAggregatePubkey = vm.envBytes("BOOTSTRAP_AGGREGATE_PUBKEY");
        config.bootstrapMembersRoot = vm.envBytes32("BOOTSTRAP_MEMBERS_ROOT");
        config.expectedBootstrapSetHash = vm.envBytes32("EXPECTED_BOOTSTRAP_SET_HASH");
        config.bootstrapMembersManifestSha256 = vm.envBytes32("BOOTSTRAP_MEMBERS_MANIFEST_SHA256");
        config.sourceBuildProvenanceSha256 = vm.envBytes32("SOURCE_BUILD_PROVENANCE_SHA256");
        config.modernStartHeight = _envUint64("MODERN_START_HEIGHT");
        config.bridgeActivationHeight = _envUint64("BRIDGE_ACTIVATION_HEIGHT");
        config.minBridgeValidators = _envUint32("MIN_BRIDGE_VALIDATORS");
        config.maxBridgeValidators = _envUint32("MAX_BRIDGE_VALIDATORS");
        config.minBridgeTotalWeight = _envUint64("MIN_BRIDGE_TOTAL_WEIGHT");
        config.minEpochDuration = vm.envUint("MIN_EPOCH_DURATION_SECONDS");
        config.maxEpochLag = vm.envUint("MAX_EPOCH_LAG_SECONDS");
        config.maxCertificateAge = vm.envUint("MAX_CERTIFICATE_AGE_SECONDS");
        config.minDepositExitWindow = vm.envUint("MIN_DEPOSIT_EXIT_WINDOW_SECONDS");
        config.bootstrapDeadline = vm.envUint("BOOTSTRAP_DEADLINE_UNIX");
    }

    function _validateConfig(ProductionBridgeConfig.Config memory config) internal view {
        ProductionBridgeConfig.validate(
            config, block.chainid, block.timestamp, _codeHash(config.originToken), _tokenDecimals(config.originToken)
        );
    }

    function _readArtifacts() internal returns (ProductionBridgeConfig.Artifacts memory artifacts) {
        artifacts.prover = vm.envAddress("PROVER_ADDRESS");
        artifacts.proverCodeHash = vm.envBytes32("EXPECTED_PROVER_RUNTIME_CODE_HASH");
        artifacts.verifier = vm.envAddress("VERIFIER_ADDRESS");
        artifacts.verifierCodeHash = vm.envBytes32("EXPECTED_VERIFIER_RUNTIME_CODE_HASH");
        artifacts.bridge = vm.envAddress("VAULT_ADDRESS");
        artifacts.bridgeCodeHash = vm.envBytes32("EXPECTED_VAULT_RUNTIME_CODE_HASH");
        artifacts.bridgeAssetId = vm.envBytes32("EXPECTED_B3_ASSET_ID");
        ProductionBridgeConfig.validateArtifacts(artifacts);
    }

    function _verifyStack(
        ProductionBridgeConfig.Config memory config,
        ProductionBridgeConfig.Artifacts memory artifacts
    ) internal view {
        ProductionBridgeConfig.validateArtifacts(artifacts);
        if (
            artifacts.prover.code.length == 0 || artifacts.verifier.code.length == 0
                || artifacts.bridge.code.length == 0 || _codeHash(artifacts.prover) != artifacts.proverCodeHash
                || _codeHash(artifacts.verifier) != artifacts.verifierCodeHash
                || _codeHash(artifacts.bridge) != artifacts.bridgeCodeHash
                || _codeHash(config.originToken) != config.expectedOriginTokenCodeHash
                || _tokenDecimals(config.originToken) != ProductionBridgeConfig.ORIGIN_TOKEN_DECIMALS
        ) revert OnchainConfigurationMismatch();

        B3FinalityVerifier verifier = B3FinalityVerifier(artifacts.verifier);
        B3StakerBridge bridge = B3StakerBridge(artifacts.bridge);
        if (
            address(verifier.prover()) != artifacts.prover || verifier.PROVER_CODE_HASH() != artifacts.proverCodeHash
                || verifier.CHAIN_DOMAIN() != config.chainDomain
                || verifier.BOOTSTRAP_SET_HASH() != config.expectedBootstrapSetHash
                || verifier.MODERN_START_HEIGHT() != config.modernStartHeight
                || verifier.BRIDGE_ACTIVATION_HEIGHT() != config.bridgeActivationHeight
                || verifier.MIN_BRIDGE_VALIDATORS() != config.minBridgeValidators
                || verifier.MAX_BRIDGE_VALIDATORS() != config.maxBridgeValidators
                || verifier.MIN_BRIDGE_TOTAL_WEIGHT() != config.minBridgeTotalWeight
                || verifier.MIN_EPOCH_DURATION() != config.minEpochDuration
                || verifier.MAX_EPOCH_LAG() != config.maxEpochLag
                || verifier.MAX_CERTIFICATE_AGE() != config.maxCertificateAge
                || verifier.MIN_DEPOSIT_EXIT_WINDOW() != config.minDepositExitWindow
                || verifier.BOOTSTRAP_DEADLINE() != config.bootstrapDeadline
                || address(bridge.verifier()) != artifacts.verifier
                || bridge.VERIFIER_CODE_HASH() != artifacts.verifierCodeHash
                || bridge.B3_CHAIN_DOMAIN() != config.chainDomain
                || bridge.ORIGIN_CHAIN_ID() != uint64(config.expectedChainId)
                || bridge.ORIGIN_TOKEN() != config.originToken || bridge.MAX_DEPOSIT_RAW() != config.maxDepositRaw
                || bridge.B3_ASSET_ID() != artifacts.bridgeAssetId
                || bridge.BRIDGE_ACTIVATION_HEIGHT() != config.bridgeActivationHeight
        ) revert OnchainConfigurationMismatch();

        B3Types.SetHeader memory header = ProductionBridgeConfig.bootstrapSet(config);
        if (B3Types.hashSetHeader(header) != verifier.BOOTSTRAP_SET_HASH()) revert OnchainConfigurationMismatch();
    }

    function _writeManifest(
        string memory path,
        ProductionBridgeConfig.Config memory config,
        ProductionBridgeConfig.Artifacts memory artifacts,
        bool deploymentInputsComplete,
        uint256 originDeploymentBlock
    ) internal {
        vm.writeJson(_serializeManifest(config, artifacts, deploymentInputsComplete, originDeploymentBlock), path);
    }

    function _serializeManifest(
        ProductionBridgeConfig.Config memory config,
        ProductionBridgeConfig.Artifacts memory artifacts,
        bool deploymentInputsComplete,
        uint256 originDeploymentBlock
    ) internal returns (string memory json) {
        string memory key = "b3_bridge_deployment";
        vm.serializeUint(key, "schema_version", 2);
        vm.serializeString(
            key, "status", deploymentInputsComplete ? "validated_deployment" : "candidate_requires_block_finalization"
        );
        vm.serializeBool(key, "deployment_inputs_complete", deploymentInputsComplete);
        // Light-client and policy pins are separate; this script cannot make
        // the complete B3 chainparams ready.
        vm.serializeBool(key, "chainparams_ready", false);
        // Contract deployment alone is never an audit or activation approval.
        vm.serializeBool(key, "production_approved", false);
        // Exact release compiler settings from foundry.toml.
        vm.serializeString(key, "solc_version", "0.8.35");
        vm.serializeString(key, "evm_version", "osaka");
        vm.serializeBool(key, "via_ir", true);
        vm.serializeBool(key, "optimizer_enabled", true);
        vm.serializeUint(key, "optimizer_runs", 200);
        vm.serializeString(key, "bytecode_hash_mode", "ipfs");
        vm.serializeBool(key, "cbor_metadata", true);
        vm.serializeString(key, "deployment_config_hash_scheme", "B3/PRODUCTION/DEPLOYMENT/CONFIG/V1");
        vm.serializeBytes32(
            key, "deployment_config_hash", ProductionBridgeConfig.deploymentConfigHash(config, artifacts)
        );
        vm.serializeBytes32(key, "source_build_provenance_sha256", config.sourceBuildProvenanceSha256);
        vm.serializeAddress(key, "deployer", config.deployer);
        vm.serializeUint(key, "ethereum_chain_id", config.expectedChainId);
        vm.serializeUint(key, "origin_deployment_block", originDeploymentBlock);
        vm.serializeBytes32(key, "b3_chain_domain", config.chainDomain);
        vm.serializeAddress(key, "origin_token", config.originToken);
        vm.serializeBytes32(key, "origin_token_runtime_code_hash", config.expectedOriginTokenCodeHash);
        vm.serializeUint(key, "origin_token_decimals", ProductionBridgeConfig.ORIGIN_TOKEN_DECIMALS);
        vm.serializeUint(key, "max_deposit_raw", config.maxDepositRaw);
        vm.serializeUint(key, "b3_max_per_block_raw", config.b3MaxPerBlockRaw);
        vm.serializeUint(key, "asset_decimals", 6);
        vm.serializeUint(key, "asset_display_decimals", 6);
        vm.serializeAddress(key, "prover", artifacts.prover);
        vm.serializeBytes32(key, "prover_runtime_code_hash", artifacts.proverCodeHash);
        vm.serializeAddress(key, "verifier", artifacts.verifier);
        vm.serializeBytes32(key, "verifier_runtime_code_hash", artifacts.verifierCodeHash);
        vm.serializeAddress(key, "vault", artifacts.bridge);
        vm.serializeBytes32(key, "vault_runtime_code_hash", artifacts.bridgeCodeHash);
        vm.serializeBytes32(key, "b3_asset_id", artifacts.bridgeAssetId);
        vm.serializeBytes32(key, "deposit_event_topic0", keccak256("Deposit(uint64,address,uint256,bytes32)"));
        vm.serializeUint(key, "bootstrap_epoch", 0);
        vm.serializeUint(key, "bootstrap_ruleset_version", 1);
        vm.serializeUint(key, "bootstrap_validator_count", 4);
        vm.serializeUint(key, "bootstrap_total_weight", 4);
        vm.serializeUint(key, "bootstrap_quorum_weight", 3);
        vm.serializeBytes(key, "bootstrap_aggregate_pubkey", config.bootstrapAggregatePubkey);
        vm.serializeBytes32(key, "bootstrap_members_root", config.bootstrapMembersRoot);
        vm.serializeBytes32(key, "bootstrap_set_hash", config.expectedBootstrapSetHash);
        vm.serializeBytes32(key, "bootstrap_members_manifest_sha256", config.bootstrapMembersManifestSha256);
        vm.serializeUint(key, "modern_start_height", config.modernStartHeight);
        vm.serializeUint(key, "bridge_activation_height", config.bridgeActivationHeight);
        vm.serializeUint(key, "minimum_bridge_validators", config.minBridgeValidators);
        vm.serializeUint(key, "maximum_bridge_validators", config.maxBridgeValidators);
        vm.serializeUint(key, "minimum_bridge_total_weight", config.minBridgeTotalWeight);
        vm.serializeUint(key, "minimum_epoch_duration_seconds", config.minEpochDuration);
        vm.serializeUint(key, "max_epoch_lag_seconds", config.maxEpochLag);
        vm.serializeUint(key, "max_certificate_age_seconds", config.maxCertificateAge);
        vm.serializeUint(key, "minimum_deposit_exit_window_seconds", config.minDepositExitWindow);
        json = vm.serializeUint(key, "bootstrap_deadline_unix", config.bootstrapDeadline);
    }

    function _codeHash(address account) internal view returns (bytes32 hash) {
        assembly {
            hash := extcodehash(account)
        }
    }

    function _tokenDecimals(address token) internal view returns (uint256 decimals) {
        (bool ok, bytes memory data) = token.staticcall(abi.encodeWithSignature("decimals()"));
        if (!ok || data.length != 32) revert TokenMetadataUnavailable();
        decimals = abi.decode(data, (uint256));
    }

    function _envUint64(string memory name) internal returns (uint64 value) {
        uint256 raw = vm.envUint(name);
        if (raw > type(uint64).max) revert IntegerOutOfRange(name);
        // Range checked immediately above.
        // forge-lint: disable-next-line(unsafe-typecast)
        value = uint64(raw);
    }

    function _envUint32(string memory name) internal returns (uint32 value) {
        uint256 raw = vm.envUint(name);
        if (raw > type(uint32).max) revert IntegerOutOfRange(name);
        // Range checked immediately above.
        // forge-lint: disable-next-line(unsafe-typecast)
        value = uint32(raw);
    }
}
