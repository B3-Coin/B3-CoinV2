// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {B3Types} from "../src/IB3FinalityProver.sol";
import {B3FinalityVerifier} from "../src/B3FinalityVerifier.sol";

/// Shared fail-closed validation for the production bridge deployment and its
/// post-broadcast manifest finalizer. Keeping this logic outside the Foundry
/// script makes the release-critical checks directly unit-testable.
library ProductionBridgeConfig {
    address internal constant ETHEREUM_MAINNET_USDT = 0xdAC17F958D2ee523a2206206994597C13D831ec7;
    bytes32 internal constant DEPLOYMENT_CONFIG_DOMAIN = keccak256("B3/PRODUCTION/DEPLOYMENT/CONFIG/V1");
    uint8 internal constant ORIGIN_TOKEN_DECIMALS = 6;
    uint16 internal constant RULESET_V1 = 1;
    uint32 internal constant BOOTSTRAP_VALIDATOR_COUNT = 4;
    uint64 internal constant BOOTSTRAP_TOTAL_WEIGHT = 4;
    uint64 internal constant BOOTSTRAP_QUORUM_WEIGHT = 3;
    // Must byte-for-byte mirror B3 consensus MAX_MONEY. Origin and B3 asset
    // decimals are both six in this deployment, so the raw-unit cap is exact.
    uint64 internal constant B3_MAX_MONEY_RAW = 662_200_000_000 * 1_000_000;

    struct Config {
        address deployer;
        uint256 expectedChainId;
        bytes32 chainDomain;
        address originToken;
        bytes32 expectedOriginTokenCodeHash;
        uint64 maxDepositRaw;
        uint64 b3MaxPerBlockRaw;
        bytes bootstrapAggregatePubkey;
        bytes32 bootstrapMembersRoot;
        bytes32 expectedBootstrapSetHash;
        /// SHA-256 of the exact reviewed public bootstrap-manifest.json bytes.
        bytes32 bootstrapMembersManifestSha256;
        /// SHA-256 of the separately reviewed source/build provenance manifest.
        bytes32 sourceBuildProvenanceSha256;
        uint64 modernStartHeight;
        uint64 bridgeActivationHeight;
        uint32 minBridgeValidators;
        uint32 maxBridgeValidators;
        uint64 minBridgeTotalWeight;
        uint256 minEpochDuration;
        uint256 maxEpochLag;
        uint256 maxCertificateAge;
        uint256 minDepositExitWindow;
        uint256 bootstrapDeadline;
    }

    struct Artifacts {
        address prover;
        bytes32 proverCodeHash;
        address verifier;
        bytes32 verifierCodeHash;
        address bridge;
        bytes32 bridgeCodeHash;
        bytes32 bridgeAssetId;
    }

    error ZeroDeployer();
    error WrongChain(uint256 expected, uint256 actual);
    error ChainIdOutOfRange();
    error ZeroChainDomain();
    error BadOriginToken();
    error WrongMainnetOriginToken(address expected, address actual);
    error OriginTokenCodeHashMismatch(bytes32 expected, bytes32 actual);
    error BadOriginTokenDecimals(uint256 actual);
    error BadDepositCap();
    error BadBootstrapSet();
    error BadHeights();
    error BadBridgeThresholds();
    error BadTimingWindow();
    error BootstrapDeadlineExpired();
    error BootstrapDeadlineTooFar();
    error BadArtifacts();
    error MissingReviewCommitment();
    error DeploymentConfigHashMismatch(bytes32 expected, bytes32 actual);

    function bootstrapSet(Config memory config) internal pure returns (B3Types.SetHeader memory) {
        return B3Types.SetHeader({
            epoch: 0,
            rulesetVersion: RULESET_V1,
            validatorCount: BOOTSTRAP_VALIDATOR_COUNT,
            totalWeight: BOOTSTRAP_TOTAL_WEIGHT,
            quorumWeight: BOOTSTRAP_QUORUM_WEIGHT,
            aggregatePubkey: config.bootstrapAggregatePubkey,
            membersRoot: config.bootstrapMembersRoot
        });
    }

    function verifierConfig(Config memory config) internal pure returns (B3FinalityVerifier.DeploymentConfig memory) {
        return B3FinalityVerifier.DeploymentConfig({
            modernStartHeight: config.modernStartHeight,
            bridgeActivationHeight: config.bridgeActivationHeight,
            minBridgeValidators: config.minBridgeValidators,
            maxBridgeValidators: config.maxBridgeValidators,
            minBridgeTotalWeight: config.minBridgeTotalWeight,
            minEpochDuration: config.minEpochDuration,
            maxEpochLag: config.maxEpochLag,
            maxCertificateAge: config.maxCertificateAge,
            minDepositExitWindow: config.minDepositExitWindow,
            bootstrapDeadline: config.bootstrapDeadline
        });
    }

    function validate(
        Config memory config,
        uint256 actualChainId,
        uint256 nowTimestamp,
        bytes32 actualTokenCodeHash,
        uint256 actualTokenDecimals
    ) internal pure {
        if (config.deployer == address(0)) revert ZeroDeployer();
        if (config.expectedChainId != actualChainId) {
            revert WrongChain(config.expectedChainId, actualChainId);
        }
        if (config.expectedChainId == 0 || config.expectedChainId > type(uint64).max) revert ChainIdOutOfRange();
        if (config.chainDomain == bytes32(0)) revert ZeroChainDomain();
        if (config.originToken == address(0) || actualTokenCodeHash == bytes32(0)) revert BadOriginToken();
        if (config.expectedChainId == 1 && config.originToken != ETHEREUM_MAINNET_USDT) {
            revert WrongMainnetOriginToken(ETHEREUM_MAINNET_USDT, config.originToken);
        }
        if (actualTokenCodeHash != config.expectedOriginTokenCodeHash) {
            revert OriginTokenCodeHashMismatch(config.expectedOriginTokenCodeHash, actualTokenCodeHash);
        }
        if (actualTokenDecimals != ORIGIN_TOKEN_DECIMALS) {
            revert BadOriginTokenDecimals(actualTokenDecimals);
        }
        if (
            config.maxDepositRaw == 0 || config.b3MaxPerBlockRaw == 0 || config.maxDepositRaw > config.b3MaxPerBlockRaw
                || config.maxDepositRaw > B3_MAX_MONEY_RAW || config.b3MaxPerBlockRaw > B3_MAX_MONEY_RAW
        ) {
            revert BadDepositCap();
        }

        B3Types.SetHeader memory header = bootstrapSet(config);
        if (
            config.expectedBootstrapSetHash == bytes32(0) || !B3Types.headerShapeValid(header)
                || B3Types.hashSetHeader(header) != config.expectedBootstrapSetHash
        ) revert BadBootstrapSet();
        if (config.bootstrapMembersManifestSha256 == bytes32(0) || config.sourceBuildProvenanceSha256 == bytes32(0)) {
            revert MissingReviewCommitment();
        }
        if (config.modernStartHeight == 0 || config.bridgeActivationHeight < config.modernStartHeight) {
            revert BadHeights();
        }
        if (
            config.minBridgeValidators < BOOTSTRAP_VALIDATOR_COUNT
                || config.maxBridgeValidators < config.minBridgeValidators
                || config.maxBridgeValidators > B3Types.MAX_PROVEN_BRIDGE_VALIDATORS || config.minBridgeTotalWeight == 0
        ) revert BadBridgeThresholds();
        if (
            config.minEpochDuration < B3Types.MIN_PROVEN_EPOCH_DURATION || config.maxEpochLag == 0
                || config.minEpochDuration > config.maxEpochLag || config.maxCertificateAge == 0
                || config.maxCertificateAge >= config.maxEpochLag || config.minDepositExitWindow == 0
                || config.minDepositExitWindow >= config.maxEpochLag
        ) revert BadTimingWindow();
        if (config.bootstrapDeadline <= nowTimestamp) {
            revert BootstrapDeadlineExpired();
        }
        if (config.bootstrapDeadline - nowTimestamp > config.maxEpochLag) revert BootstrapDeadlineTooFar();
    }

    function validateArtifacts(Artifacts memory artifacts) internal pure {
        if (
            artifacts.prover == address(0) || artifacts.proverCodeHash == bytes32(0) || artifacts.verifier == address(0)
                || artifacts.verifierCodeHash == bytes32(0) || artifacts.bridge == address(0)
                || artifacts.bridgeCodeHash == bytes32(0) || artifacts.bridgeAssetId == bytes32(0)
        ) revert BadArtifacts();
    }

    /// Domain-separated commitment reviewed between Deploy and Finalize. Hashing
    /// the ABI structs means every config and artifact field participates, while
    /// the two embedded SHA-256 values bind the public bootstrap input and the
    /// independently preserved source/build provenance manifest.
    function deploymentConfigHash(Config memory config, Artifacts memory artifacts) internal pure returns (bytes32) {
        return keccak256(
            abi.encode(DEPLOYMENT_CONFIG_DOMAIN, keccak256(abi.encode(config)), keccak256(abi.encode(artifacts)))
        );
    }

    function validateDeploymentConfigHash(
        Config memory config,
        Artifacts memory artifacts,
        bytes32 expectedDeploymentConfigHash
    ) internal pure {
        bytes32 actual = deploymentConfigHash(config, artifacts);
        if (expectedDeploymentConfigHash == bytes32(0) || actual != expectedDeploymentConfigHash) {
            revert DeploymentConfigHashMismatch(expectedDeploymentConfigHash, actual);
        }
    }
}
