// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {TestBase} from "./TestBase.sol";
import {ProductionBridgeConfig} from "../script/ProductionBridgeConfig.sol";
import {ProductionBridgeScript} from "../script/ProductionBridgeScript.sol";
import {B3Types, IB3FinalityProver} from "../src/IB3FinalityProver.sol";
import {BlsCertificateProver} from "../src/BlsCertificateProver.sol";
import {B3FinalityVerifier} from "../src/B3FinalityVerifier.sol";
import {B3StakerBridge} from "../src/B3StakerBridge.sol";

interface JsonVm {
    function parseJson(string calldata json, string calldata key) external pure returns (bytes memory abiEncodedData);
}

contract SixDecimalOriginToken {
    function decimals() external pure returns (uint8) {
        return 6;
    }
}

contract ProductionBridgeConfigHarness {
    function validate(
        ProductionBridgeConfig.Config calldata config,
        uint256 actualChainId,
        uint256 nowTimestamp,
        bytes32 actualTokenCodeHash,
        uint256 actualTokenDecimals
    ) external pure {
        ProductionBridgeConfig.validate(config, actualChainId, nowTimestamp, actualTokenCodeHash, actualTokenDecimals);
    }

    function validateArtifacts(ProductionBridgeConfig.Artifacts calldata artifacts) external pure {
        ProductionBridgeConfig.validateArtifacts(artifacts);
    }

    function deploymentConfigHash(
        ProductionBridgeConfig.Config calldata config,
        ProductionBridgeConfig.Artifacts calldata artifacts
    ) external pure returns (bytes32) {
        return ProductionBridgeConfig.deploymentConfigHash(config, artifacts);
    }

    function validateDeploymentConfigHash(
        ProductionBridgeConfig.Config calldata config,
        ProductionBridgeConfig.Artifacts calldata artifacts,
        bytes32 expected
    ) external pure {
        ProductionBridgeConfig.validateDeploymentConfigHash(config, artifacts, expected);
    }
}

contract ProductionBridgeManifestHarness is ProductionBridgeScript {
    function serializeManifest(
        ProductionBridgeConfig.Config calldata config,
        ProductionBridgeConfig.Artifacts calldata artifacts,
        bool complete,
        uint256 deploymentBlock
    ) external returns (string memory) {
        return _serializeManifest(config, artifacts, complete, deploymentBlock);
    }

    function verifyStack(
        ProductionBridgeConfig.Config calldata config,
        ProductionBridgeConfig.Artifacts calldata artifacts
    ) external view {
        _verifyStack(config, artifacts);
    }
}

contract ProductionBridgeConfigTest is TestBase {
    ProductionBridgeConfigHarness private harness;
    ProductionBridgeManifestHarness private manifestHarness;
    ProductionBridgeConfig.Config private config;
    bytes32 private constant TOKEN_HASH = keccak256("reviewed-usdt-runtime");
    uint256 private constant NOW = 1_800_000_000;

    function setUp() public {
        harness = new ProductionBridgeConfigHarness();
        manifestHarness = new ProductionBridgeManifestHarness();
        config.deployer = address(0x1001);
        config.expectedChainId = 1;
        config.chainDomain = keccak256("b3-mainnet-domain");
        config.originToken = ProductionBridgeConfig.ETHEREUM_MAINNET_USDT;
        config.expectedOriginTokenCodeHash = TOKEN_HASH;
        config.maxDepositRaw = 1_000_000_000;
        config.b3MaxPerBlockRaw = 1_000_000_000;
        config.bootstrapAggregatePubkey = abi.encodePacked(bytes1(0x80), bytes32(0), bytes15(uint120(1)));
        config.bootstrapMembersRoot = keccak256("four-reviewed-members");
        config.bootstrapMembersManifestSha256 = sha256("reviewed-public-bootstrap-manifest");
        config.sourceBuildProvenanceSha256 = sha256("reviewed-source-build-provenance");
        config.modernStartHeight = 811_001;
        config.bridgeActivationHeight = 812_000;
        config.minBridgeValidators = 4;
        config.maxBridgeValidators = 64;
        config.minBridgeTotalWeight = 120_000;
        config.minEpochDuration = 1 days;
        config.maxEpochLag = 14 days;
        config.maxCertificateAge = 1 hours;
        config.minDepositExitWindow = 2 days;
        config.bootstrapDeadline = NOW + 7 days;
        config.expectedBootstrapSetHash = B3Types.hashSetHeader(ProductionBridgeConfig.bootstrapSet(config));
    }

    function testAcceptsFullyPinnedConfiguration() public view {
        harness.validate(config, 1, NOW, TOKEN_HASH, 6);
    }

    function testRejectsWrongChain() public {
        _expectValidationError(ProductionBridgeConfig.WrongChain.selector, 11_155_111, TOKEN_HASH, 6);
    }

    function testRejectsTokenRuntimeMismatch() public {
        _expectValidationError(
            ProductionBridgeConfig.OriginTokenCodeHashMismatch.selector, 1, keccak256("other-runtime"), 6
        );
    }

    function testRejectsNonCanonicalTokenOnEthereumMainnet() public {
        config.originToken = address(0x1002);
        _expectValidationError(ProductionBridgeConfig.WrongMainnetOriginToken.selector, 1, TOKEN_HASH, 6);
    }

    function testAllowsReviewedNonMainnetSixDecimalToken() public view {
        ProductionBridgeConfig.Config memory nonMainnet = config;
        nonMainnet.expectedChainId = 11_155_111;
        nonMainnet.originToken = address(0x1002);
        harness.validate(nonMainnet, 11_155_111, NOW, TOKEN_HASH, 6);
    }

    function testRejectsNonSixDecimalToken() public {
        _expectValidationError(ProductionBridgeConfig.BadOriginTokenDecimals.selector, 1, TOKEN_HASH, 18);
    }

    function testRejectsZeroDepositCap() public {
        config.maxDepositRaw = 0;
        vm.expectRevert(ProductionBridgeConfig.BadDepositCap.selector);
        harness.validate(config, 1, NOW, TOKEN_HASH, 6);
    }

    function testRejectsDepositCapAboveB3PerBlockLimit() public {
        config.maxDepositRaw = config.b3MaxPerBlockRaw + 1;
        vm.expectRevert(ProductionBridgeConfig.BadDepositCap.selector);
        harness.validate(config, 1, NOW, TOKEN_HASH, 6);
    }

    function testRejectsZeroB3PerBlockLimit() public {
        config.b3MaxPerBlockRaw = 0;
        vm.expectRevert(ProductionBridgeConfig.BadDepositCap.selector);
        harness.validate(config, 1, NOW, TOKEN_HASH, 6);
    }

    function testAcceptsExactB3ConsensusMoneyRangeLimit() public {
        config.maxDepositRaw = ProductionBridgeConfig.B3_MAX_MONEY_RAW;
        config.b3MaxPerBlockRaw = ProductionBridgeConfig.B3_MAX_MONEY_RAW;
        harness.validate(config, 1, NOW, TOKEN_HASH, 6);
    }

    function testRejectsDepositCapAboveB3ConsensusMoneyRange() public {
        config.maxDepositRaw = ProductionBridgeConfig.B3_MAX_MONEY_RAW + 1;
        config.b3MaxPerBlockRaw = config.maxDepositRaw;
        vm.expectRevert(ProductionBridgeConfig.BadDepositCap.selector);
        harness.validate(config, 1, NOW, TOKEN_HASH, 6);
    }

    function testRejectsB3PerBlockLimitAboveConsensusMoneyRange() public {
        config.b3MaxPerBlockRaw = ProductionBridgeConfig.B3_MAX_MONEY_RAW + 1;
        vm.expectRevert(ProductionBridgeConfig.BadDepositCap.selector);
        harness.validate(config, 1, NOW, TOKEN_HASH, 6);
    }

    function testRejectsBootstrapHeaderNotMatchingReviewedHash() public {
        config.bootstrapMembersRoot = keccak256("different-members");
        vm.expectRevert(ProductionBridgeConfig.BadBootstrapSet.selector);
        harness.validate(config, 1, NOW, TOKEN_HASH, 6);
    }

    function testRejectsMissingBootstrapOrBuildProvenanceCommitment() public {
        config.bootstrapMembersManifestSha256 = bytes32(0);
        vm.expectRevert(ProductionBridgeConfig.MissingReviewCommitment.selector);
        harness.validate(config, 1, NOW, TOKEN_HASH, 6);

        config.bootstrapMembersManifestSha256 = sha256("reviewed-public-bootstrap-manifest");
        config.sourceBuildProvenanceSha256 = bytes32(0);
        vm.expectRevert(ProductionBridgeConfig.MissingReviewCommitment.selector);
        harness.validate(config, 1, NOW, TOKEN_HASH, 6);
    }

    function testRejectsExpiredBootstrapDeadline() public {
        config.bootstrapDeadline = NOW;
        vm.expectRevert(ProductionBridgeConfig.BootstrapDeadlineExpired.selector);
        harness.validate(config, 1, NOW, TOKEN_HASH, 6);
    }

    function testRejectsDeadlineBeyondWeakSubjectivityWindow() public {
        config.bootstrapDeadline = NOW + config.maxEpochLag + 1;
        vm.expectRevert(ProductionBridgeConfig.BootstrapDeadlineTooFar.selector);
        harness.validate(config, 1, NOW, TOKEN_HASH, 6);
    }

    function testRejectsUnbenchmarkedBridgeValidatorLimit() public {
        config.maxBridgeValidators = B3Types.MAX_PROVEN_BRIDGE_VALIDATORS + 1;
        vm.expectRevert(ProductionBridgeConfig.BadBridgeThresholds.selector);
        harness.validate(config, 1, NOW, TOKEN_HASH, 6);
    }

    function testRejectsInvalidEpochDurationBounds() public {
        config.minEpochDuration = 0;
        vm.expectRevert(ProductionBridgeConfig.BadTimingWindow.selector);
        harness.validate(config, 1, NOW, TOKEN_HASH, 6);

        config.minEpochDuration = B3Types.MIN_PROVEN_EPOCH_DURATION - 1;
        vm.expectRevert(ProductionBridgeConfig.BadTimingWindow.selector);
        harness.validate(config, 1, NOW, TOKEN_HASH, 6);

        config.minEpochDuration = config.maxEpochLag + 1;
        vm.expectRevert(ProductionBridgeConfig.BadTimingWindow.selector);
        harness.validate(config, 1, NOW, TOKEN_HASH, 6);
    }

    function testRejectsIncompleteArtifactManifest() public {
        ProductionBridgeConfig.Artifacts memory artifacts;
        artifacts.prover = address(0x2001);
        artifacts.proverCodeHash = keccak256("prover");
        artifacts.verifier = address(0x2002);
        artifacts.verifierCodeHash = keccak256("verifier");
        artifacts.bridge = address(0x2003);
        artifacts.bridgeCodeHash = keccak256("bridge");
        vm.expectRevert(ProductionBridgeConfig.BadArtifacts.selector);
        harness.validateArtifacts(artifacts);
    }

    function testReviewedDeploymentConfigCommitmentMatchesExactly() public view {
        ProductionBridgeConfig.Artifacts memory artifacts = _artifacts();
        bytes32 approved = harness.deploymentConfigHash(config, artifacts);
        harness.validateDeploymentConfigHash(config, artifacts, approved);
    }

    function testRejectsDeploymentConfigCommitmentMismatch() public {
        ProductionBridgeConfig.Artifacts memory artifacts = _artifacts();
        bytes32 approved = harness.deploymentConfigHash(config, artifacts);

        ProductionBridgeConfig.Config memory changedConfig = config;
        changedConfig.sourceBuildProvenanceSha256 = sha256("different-build");
        _expectDeploymentCommitmentError(changedConfig, artifacts, approved);

        ProductionBridgeConfig.Artifacts memory changedArtifacts = artifacts;
        changedArtifacts.verifierCodeHash = keccak256("different-verifier");
        _expectDeploymentCommitmentError(config, changedArtifacts, approved);

        _expectDeploymentCommitmentError(config, artifacts, bytes32(0));
    }

    function testManifestCannotMarkCandidateChainparamsComplete() public {
        ProductionBridgeConfig.Artifacts memory artifacts = _artifacts();
        string memory json = manifestHarness.serializeManifest(config, artifacts, false, 0);
        assertFalse(abi.decode(_jsonVm().parseJson(json, ".deployment_inputs_complete"), (bool)));
        assertFalse(abi.decode(_jsonVm().parseJson(json, ".chainparams_ready"), (bool)));
        assertFalse(abi.decode(_jsonVm().parseJson(json, ".production_approved"), (bool)));
        assertEq(abi.decode(_jsonVm().parseJson(json, ".origin_deployment_block"), (uint256)), 0);
        assertEq(abi.decode(_jsonVm().parseJson(json, ".vault_runtime_code_hash"), (bytes32)), artifacts.bridgeCodeHash);
    }

    function testFinalManifestCarriesExactDeploymentCoordinates() public {
        ProductionBridgeConfig.Artifacts memory artifacts = _artifacts();
        string memory json = manifestHarness.serializeManifest(config, artifacts, true, 25_900_000);
        assertEq(abi.decode(_jsonVm().parseJson(json, ".schema_version"), (uint256)), 2);
        assertTrue(abi.decode(_jsonVm().parseJson(json, ".deployment_inputs_complete"), (bool)));
        assertFalse(abi.decode(_jsonVm().parseJson(json, ".chainparams_ready"), (bool)));
        assertFalse(abi.decode(_jsonVm().parseJson(json, ".production_approved"), (bool)));
        assertEq(abi.decode(_jsonVm().parseJson(json, ".origin_deployment_block"), (uint256)), 25_900_000);
        assertEq(abi.decode(_jsonVm().parseJson(json, ".ethereum_chain_id"), (uint256)), config.expectedChainId);
        assertEq(
            uint256(uint160(address(abi.decode(_jsonVm().parseJson(json, ".origin_token"), (address))))),
            uint256(uint160(config.originToken))
        );
        assertEq(
            abi.decode(_jsonVm().parseJson(json, ".origin_token_runtime_code_hash"), (bytes32)),
            config.expectedOriginTokenCodeHash
        );
        assertEq(
            uint256(uint160(address(abi.decode(_jsonVm().parseJson(json, ".prover"), (address))))),
            uint256(uint160(artifacts.prover))
        );
        assertEq(
            abi.decode(_jsonVm().parseJson(json, ".prover_runtime_code_hash"), (bytes32)), artifacts.proverCodeHash
        );
        assertEq(
            uint256(uint160(address(abi.decode(_jsonVm().parseJson(json, ".verifier"), (address))))),
            uint256(uint160(artifacts.verifier))
        );
        assertEq(
            abi.decode(_jsonVm().parseJson(json, ".verifier_runtime_code_hash"), (bytes32)), artifacts.verifierCodeHash
        );
        assertEq(
            uint256(uint160(address(abi.decode(_jsonVm().parseJson(json, ".vault"), (address))))),
            uint256(uint160(artifacts.bridge))
        );
        assertEq(abi.decode(_jsonVm().parseJson(json, ".vault_runtime_code_hash"), (bytes32)), artifacts.bridgeCodeHash);
        assertEq(abi.decode(_jsonVm().parseJson(json, ".b3_asset_id"), (bytes32)), artifacts.bridgeAssetId);
        assertEq(
            abi.decode(_jsonVm().parseJson(json, ".bootstrap_members_manifest_sha256"), (bytes32)),
            config.bootstrapMembersManifestSha256
        );
        assertEq(
            abi.decode(_jsonVm().parseJson(json, ".source_build_provenance_sha256"), (bytes32)),
            config.sourceBuildProvenanceSha256
        );
        assertEq(
            abi.decode(_jsonVm().parseJson(json, ".deployment_config_hash"), (bytes32)),
            harness.deploymentConfigHash(config, artifacts)
        );
        assertEq(abi.decode(_jsonVm().parseJson(json, ".max_deposit_raw"), (uint256)), config.maxDepositRaw);
        assertEq(abi.decode(_jsonVm().parseJson(json, ".b3_max_per_block_raw"), (uint256)), config.b3MaxPerBlockRaw);
        assertEq(
            abi.decode(_jsonVm().parseJson(json, ".maximum_bridge_validators"), (uint256)), config.maxBridgeValidators
        );
        assertEq(
            abi.decode(_jsonVm().parseJson(json, ".minimum_epoch_duration_seconds"), (uint256)), config.minEpochDuration
        );
    }

    function testDependencyOrderedStackMatchesEveryReviewedPin() public {
        SixDecimalOriginToken token = new SixDecimalOriginToken();
        ProductionBridgeConfig.Config memory liveConfig = config;
        liveConfig.expectedChainId = block.chainid;
        liveConfig.originToken = address(token);
        liveConfig.expectedOriginTokenCodeHash = address(token).codehash;
        liveConfig.bootstrapDeadline = block.timestamp + 7 days;

        BlsCertificateProver prover = new BlsCertificateProver();
        B3FinalityVerifier verifier = _deployVerifier(liveConfig, prover);
        B3StakerBridge bridge = new B3StakerBridge(
            verifier, address(verifier).codehash, liveConfig.chainDomain, address(token), liveConfig.maxDepositRaw
        );
        ProductionBridgeConfig.Artifacts memory artifacts = ProductionBridgeConfig.Artifacts({
            prover: address(prover),
            proverCodeHash: address(prover).codehash,
            verifier: address(verifier),
            verifierCodeHash: address(verifier).codehash,
            bridge: address(bridge),
            bridgeCodeHash: address(bridge).codehash,
            bridgeAssetId: bridge.B3_ASSET_ID()
        });

        manifestHarness.verifyStack(liveConfig, artifacts);

        // The post-deployment finalizer must not approve a manifest whose
        // reviewed deposit cap differs from the immutable value on the vault.
        liveConfig.maxDepositRaw += 1;
        vm.expectRevert(ProductionBridgeScript.OnchainConfigurationMismatch.selector);
        manifestHarness.verifyStack(liveConfig, artifacts);
    }

    function _expectValidationError(
        bytes4 expectedSelector,
        uint256 actualChainId,
        bytes32 tokenHash,
        uint256 tokenDecimals
    ) private {
        (bool ok, bytes memory result) = address(harness)
            .call(
                abi.encodeCall(
                    ProductionBridgeConfigHarness.validate, (config, actualChainId, NOW, tokenHash, tokenDecimals)
                )
            );
        assertFalse(ok);
        assertTrue(result.length >= 4);
        bytes4 actualSelector;
        assembly {
            actualSelector := mload(add(result, 32))
        }
        assertTrue(actualSelector == expectedSelector);
    }

    function _expectDeploymentCommitmentError(
        ProductionBridgeConfig.Config memory testedConfig,
        ProductionBridgeConfig.Artifacts memory artifacts,
        bytes32 expected
    ) private {
        (bool ok, bytes memory result) = address(harness)
            .call(
                abi.encodeCall(
                    ProductionBridgeConfigHarness.validateDeploymentConfigHash, (testedConfig, artifacts, expected)
                )
            );
        assertFalse(ok);
        assertTrue(result.length >= 4);
        bytes4 actualSelector;
        assembly {
            actualSelector := mload(add(result, 32))
        }
        assertTrue(actualSelector == ProductionBridgeConfig.DeploymentConfigHashMismatch.selector);
    }

    function _deployVerifier(ProductionBridgeConfig.Config memory liveConfig, BlsCertificateProver prover)
        private
        returns (B3FinalityVerifier)
    {
        return new B3FinalityVerifier(
            liveConfig.chainDomain,
            ProductionBridgeConfig.bootstrapSet(liveConfig),
            liveConfig.expectedBootstrapSetHash,
            IB3FinalityProver(address(prover)),
            address(prover).codehash,
            ProductionBridgeConfig.verifierConfig(liveConfig)
        );
    }

    function _artifacts() private pure returns (ProductionBridgeConfig.Artifacts memory artifacts) {
        artifacts.prover = address(0x2001);
        artifacts.proverCodeHash = keccak256("prover");
        artifacts.verifier = address(0x2002);
        artifacts.verifierCodeHash = keccak256("verifier");
        artifacts.bridge = address(0x2003);
        artifacts.bridgeCodeHash = keccak256("bridge");
        artifacts.bridgeAssetId = keccak256("asset");
    }

    function _jsonVm() private pure returns (JsonVm) {
        return JsonVm(address(uint160(uint256(keccak256("hevm cheat code")))));
    }
}
