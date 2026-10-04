// SPDX-License-Identifier: MIT
pragma solidity ^0.8.28;

import {RelayFixtureV2} from "./utils/RelayFixtureV2.sol";
import {BlsTestSignerV2 as Signer} from "./utils/BlsTestSignerV2.sol";
import {B3Types} from "../../src/IB3FinalityProver.sol";
import {B3TypesV2} from "../../src/v2/B3TypesV2.sol";
import {B3FinalityVerifierV2} from "../../src/v2/B3FinalityVerifierV2.sol";
import {B3VaultV2} from "../../src/v2/B3VaultV2.sol";
import {DeployV2} from "../../script/DeployV2.s.sol";

/// Cheat codes this suite needs beyond VmV2.
interface DeployCheatsV2 {
    function getNonce(address account) external view returns (uint64 nonce);
    function computeCreateAddress(address deployer, uint256 nonce) external pure returns (address);
    function parseJsonAddressArray(string calldata json, string calldata key) external pure returns (address[] memory);
}

/// script/DeployV2.s.sol, run in process on chain 31337 (contracts spec §3.1,
/// §4, §5; regtest plan §3.3, OI-10).
///
/// setUp writes the required B3V2_* variables, identical in every test,
/// because forge runs tests in parallel and the environment is shared by the
/// whole process; optional variables stay unset so their profile-R defaults
/// apply. Variants go through deploy(Params) instead of the environment.
///
/// A constructor revert inside the script's broadcast leaves Foundry's
/// broadcast open, so such a case is always the last call of its test.
contract DeployV2Test is RelayFixtureV2 {
    DeployCheatsV2 private constant CHEATS = DeployCheatsV2(address(uint160(uint256(keccak256("hevm cheat code")))));

    address private constant DEPLOYER = address(0xDE9107E2);
    address private constant USDT_MAINNET = 0xdAC17F958D2ee523a2206206994597C13D831ec7;
    uint256 private constant NOW_MAINNET = 1_800_000_000;
    B3FinalityVerifierV2.BootstrapMode private constant START_CERT =
    B3FinalityVerifierV2.BootstrapMode.START_CERTIFICATE;

    DeployV2 private script;

    function setUp() public override {
        super.setUp();
        script = new DeployV2();
        vm.setEnv("B3V2_DEPLOYER", vm.toString(DEPLOYER));
        vm.setEnv("B3V2_CHAIN_DOMAIN", vm.toString(DOMAIN));
        vm.setEnv("B3V2_GENESIS_HEIGHT", vm.toString(uint256(G)));
        vm.setEnv("B3V2_GENESIS_BLOCK_HASH", vm.toString(genesisHash()));
        vm.setEnv("B3V2_GENESIS_SET_HASH", vm.toString(genesisSetHash));
        vm.setEnv("B3V2_START_EPOCH", "0");
        vm.setEnv("B3V2_START_SET_HASH", vm.toString(genesisSetHash));
        vm.setEnv("B3V2_BOOTSTRAP_MODE", "0");
        vm.setEnv("B3V2_BRIDGE_ACTIVATION_HEIGHT", vm.toString(uint256(V)));
        vm.setEnv("B3V2_ORIGIN_TOKEN", vm.toString(address(usdt)));
        vm.setEnv("B3V2_BOOTSTRAP_AGGREGATE_PUBKEY", vm.toString(bootstrapHeader.aggregatePubkey));
        vm.setEnv("B3V2_BOOTSTRAP_MEMBERS_ROOT", vm.toString(bootstrapHeader.membersRoot));
        vm.setEnv("B3V2_BOOTSTRAP_SET_HASH", vm.toString(bootstrapSetHash));
        vm.setEnv("B3V2_WATCHERS", string.concat(vm.toString(WATCHER_A), ",", vm.toString(WATCHER_B)));
    }

    // ===================================================================== run

    function test_Run_DeploysStackMatchingEnvironment() public {
        DeployV2.Deployment memory d = script.run();

        // Four CREATEs by the deployer at nonces 0..3; the factory is nonce 4, still codeless.
        assertEq(uint256(d.deployerNonce), 0);
        assertEq(address(d.prover), CHEATS.computeCreateAddress(DEPLOYER, 0));
        assertEq(address(d.verifier), CHEATS.computeCreateAddress(DEPLOYER, 1));
        assertEq(address(d.vault), CHEATS.computeCreateAddress(DEPLOYER, 2));
        assertEq(address(d.batch), CHEATS.computeCreateAddress(DEPLOYER, 3));
        assertEq(uint256(CHEATS.getNonce(DEPLOYER)), 4, "exactly four deployments");
        assertTrue(d.forwarderFactoryPredicted);
        assertEq(d.forwarderFactory, CHEATS.computeCreateAddress(DEPLOYER, 4));
        assertEq(d.forwarderFactory.code.length, 0);

        // Unmodified code: the same runtime as the fixture's own deployments.
        assertEq(d.proverCodeHash, address(prover).codehash, "BlsDigestProver runtime");
        assertEq(d.batchCodeHash, address(batch).codehash, "B3RelayBatch runtime");
        assertEq(d.verifierCodeHash, address(d.verifier).codehash);
        assertEq(d.vaultCodeHash, address(d.vault).codehash);

        B3FinalityVerifierV2 v = d.verifier;
        assertEq(v.CHAIN_DOMAIN(), DOMAIN);
        assertEq(uint256(v.GENESIS_HEIGHT()), G);
        assertEq(v.GENESIS_BLOCK_HASH(), genesisHash());
        assertEq(v.GENESIS_SET_HASH(), genesisSetHash);
        assertEq(v.LINEAGE_ID(), lineageIdV, "recomputed lineage id");
        assertEq(uint256(v.START_EPOCH()), 0);
        assertEq(v.START_SET_HASH(), genesisSetHash);
        assertTrue(v.BOOTSTRAP_MODE() == B3FinalityVerifierV2.BootstrapMode.K_OF_N);
        assertEq(v.BOOTSTRAP_SET_HASH(), bootstrapSetHash);
        assertEq(address(v.PROVER()), address(d.prover));
        assertEq(v.PROVER_CODE_HASH(), d.proverCodeHash);
        assertEq(uint256(v.BRIDGE_ACTIVATION_HEIGHT()), V);
        assertEq(uint256(v.MIN_BRIDGE_VALIDATORS()), 4);
        assertEq(uint256(v.MAX_BRIDGE_VALIDATORS()), 64);
        assertEq(uint256(v.MIN_BRIDGE_TOTAL_WEIGHT()), 900);
        assertEq(v.MIN_EPOCH_DURATION(), MIN_EPOCH);
        assertEq(v.MAX_EPOCH_LAG(), LAG);
        assertEq(v.DEPOSIT_CLOSE_AFTER(), CLOSE);
        assertEq(v.MAX_CERTIFICATE_AGE(), AGE);
        assertEq(v.INIT_DEADLINE(), T0 + INIT_WINDOW);
        assertEq(d.initDeadline, T0 + INIT_WINDOW);
        assertEq(v.REVIEW_WINDOW(), REVIEW);
        address[] memory watchers = v.watchers();
        assertEq(watchers.length, 2);
        assertEq(watchers[0], WATCHER_A);
        assertEq(watchers[1], WATCHER_B);
        assertEq(v.status(), B3TypesV2.STATUS_UNINITIALIZED);

        B3VaultV2 vault = d.vault;
        assertEq(address(vault.VERIFIER()), address(v));
        assertEq(vault.VERIFIER_CODE_HASH(), address(v).codehash);
        assertEq(vault.B3_CHAIN_DOMAIN(), DOMAIN);
        assertEq(vault.ORIGIN_TOKEN(), address(usdt));
        assertEq(vault.TOKEN_CODE_HASH(), address(usdt).codehash);
        assertEq(vault.FORWARDER_FACTORY(), d.forwarderFactory);
        assertEq(vault.MAX_DEPOSIT_RAW(), CAP);
        assertEq(uint256(vault.BRIDGE_ACTIVATION_HEIGHT()), V);
        assertEq(uint256(vault.ORIGIN_CHAIN_ID()), 31_337);
        bytes32 assetId = B3TypesV2.assetId(DOMAIN, 31_337, address(vault), address(usdt));
        assertEq(vault.B3_ASSET_ID(), assetId);
        assertEq(d.assetId, assetId);
        assertEq(vault.PREDECESSOR(), address(0));
        assertEq(d.tokenCodeHash, address(usdt).codehash);
    }

    /// The deployed stack works end to end: the env-pinned bootstrap keys
    /// initialize it 3-of-4, an epoch handover at V certifies a real leaf V2
    /// root, an EOA deposits, and the deployed batch releases the leaf.
    function test_Run_StackBootstrapsKOfNAndCarriesTheRoute() public {
        DeployV2.Deployment memory d = script.run();
        B3FinalityVerifierV2 v = d.verifier;
        Signer.TestSet memory s1 = lineageSet(1);
        Signer.TestSet memory s2 = lineageSet(2);

        bootstrap(v, 1, s1);
        assertEq(v.status(), B3TypesV2.STATUS_LIVE);
        assertEq(v.currentSetHash(), genesisSetHash);
        assertEq(v.nextSetHash(), s1.setHash);
        assertEq(uint256(v.lastCertificateTime()), 0);
        assertFalse(d.vault.depositGateOpen(), "bootstrap alone opens nothing");

        B3TypesV2.WithdrawalV2[] memory ws = new B3TypesV2.WithdrawalV2[](1);
        ws[0] = withdrawal(0, CAROL, 3 * UNIT, V);
        bytes32[] memory leaves = treeOf(d.vault, ws);
        bytes32 root = rootOf(leaves, 1);

        vm.warp(T0 + REVIEW);
        submit(v, canonicalCert(V_INDEX, 1, s1, s2, root));
        assertEq(uint256(v.currentEpoch()), 1);
        assertEq(uint256(v.acceptedRootHeight(root)), V);
        assertTrue(v.depositsOpen());
        assertTrue(d.vault.depositGateOpen());

        depositAs(d.vault, ALICE, 5 * UNIT);
        assertEq(d.vault.locked(), 5 * UNIT);

        (uint256 certsAccepted, uint256 releasesPaid) =
            d.batch.relay(v, noCerts(), CERT_GAS, address(d.vault), items(item(leaves, 1, ws[0])), RELEASE_GAS);
        assertEq(certsAccepted, 0);
        assertEq(releasesPaid, 1);
        assertEq(usdt.balanceOf(CAROL), 3 * UNIT);
        assertTrue(d.vault.released(0));
        assertEq(d.vault.locked(), 2 * UNIT);
        assertEq(usdt.balanceOf(address(d.batch)), 0);
    }

    function test_Run_RefusesEveryChainButRegtest() public {
        uint256[5] memory chainIds = [uint256(1), 11_155_111, 42_161, 17_000, 0];
        for (uint256 i = 0; i < chainIds.length; ++i) {
            vm.chainId(chainIds[i]);
            vm.expectRevert(abi.encodeWithSelector(DeployV2.NotRegtest.selector, chainIds[i]));
            script.run();
        }
        assertEq(uint256(CHEATS.getNonce(DEPLOYER)), 0, "nothing deployed");
    }

    function test_Deploy_RefusesEveryChainButRegtest() public {
        DeployV2.Params memory p = script.readParams();
        vm.chainId(1);
        vm.expectRevert(abi.encodeWithSelector(DeployV2.NotRegtest.selector, 1));
        script.deploy(p);
        vm.chainId(11_155_111);
        vm.expectRevert(abi.encodeWithSelector(DeployV2.NotRegtest.selector, 11_155_111));
        script.deploy(p);
        assertEq(uint256(CHEATS.getNonce(DEPLOYER)), 0, "nothing deployed");
    }

    // ============================================================= parameters

    function test_ReadParams_RequiredValuesAndProfileRDefaults() public view {
        DeployV2.Params memory p = script.readParams();
        assertEq(p.deployer, DEPLOYER);
        assertEq(p.chainDomain, DOMAIN);
        assertEq(uint256(p.genesisHeight), G);
        assertEq(p.genesisBlockHash, genesisHash());
        assertEq(p.genesisSetHash, genesisSetHash);
        assertEq(p.lineageId, lineageIdV, "default lineage id is recomputed");
        assertEq(uint256(p.startEpoch), 0);
        assertEq(p.startSetHash, genesisSetHash);
        assertTrue(p.bootstrapMode == B3FinalityVerifierV2.BootstrapMode.K_OF_N);
        assertEq(p.bootstrapAggregatePubkey, bootstrapHeader.aggregatePubkey);
        assertEq(p.bootstrapMembersRoot, bootstrapHeader.membersRoot);
        assertEq(p.bootstrapSetHash, bootstrapSetHash);
        assertEq(uint256(p.bridgeActivationHeight), V);
        assertEq(p.originToken, address(usdt));
        assertEq(p.watchers.length, 2);
        assertEq(p.watchers[0], WATCHER_A);
        assertEq(p.watchers[1], WATCHER_B);

        assertEq(uint256(p.minBridgeValidators), 4);
        assertEq(uint256(p.maxBridgeValidators), 64);
        assertEq(uint256(p.minBridgeTotalWeight), 900);
        assertEq(p.windows.minEpochDuration, MIN_EPOCH);
        assertEq(p.windows.maxEpochLag, LAG);
        assertEq(p.windows.depositCloseAfter, CLOSE);
        assertEq(p.windows.maxCertificateAge, AGE);
        assertEq(p.windows.initWindow, INIT_WINDOW);
        assertEq(p.windows.reviewWindow, REVIEW);
        assertEq(p.maxDepositRaw, CAP);
        assertEq(p.tokenCodeHash, bytes32(0), "resolved to the token's code hash at deployment");
        assertEq(p.forwarderFactory, address(0), "resolved to the predicted address at deployment");
        assertEq(bytes(p.manifestPath).length, 0);

        // The script's K_OF_N header is the fixture's bootstrap key header.
        assertEq(
            B3Types.hashSetHeader(script.bootstrapHeader(p.bootstrapAggregatePubkey, p.bootstrapMembersRoot)),
            bootstrapSetHash
        );
    }

    function test_ParseBootstrapMode() public {
        assertTrue(script.parseBootstrapMode(0) == B3FinalityVerifierV2.BootstrapMode.K_OF_N);
        assertTrue(script.parseBootstrapMode(1) == START_CERT);
        vm.expectRevert(abi.encodeWithSelector(DeployV2.BadParam.selector, "B3V2_BOOTSTRAP_MODE"));
        script.parseBootstrapMode(2);
        vm.expectRevert(abi.encodeWithSelector(DeployV2.BadParam.selector, "B3V2_BOOTSTRAP_MODE"));
        script.parseBootstrapMode(type(uint256).max);
    }

    function test_Deploy_StartCertificateMode() public {
        DeployV2.Params memory p = script.readParams();
        p.bootstrapMode = START_CERT;
        p.bootstrapAggregatePubkey = "";
        p.bootstrapMembersRoot = bytes32(0);
        p.bootstrapSetHash = bytes32(0);
        DeployV2.Deployment memory d = script.deploy(p);
        B3FinalityVerifierV2 v = d.verifier;
        assertTrue(v.BOOTSTRAP_MODE() == START_CERT);
        assertEq(v.BOOTSTRAP_SET_HASH(), bytes32(0));

        Signer.TestSet memory s0 = lineageSet(0);
        Signer.TestSet memory s1 = lineageSet(1);
        expectInitRevert(
            v, initArgs(1, bootstrapKeys(), s0, s1, Signer.ids(3)), B3FinalityVerifierV2.BadSigningSet.selector
        );
        doInit(v, initArgs(1, s0, s0, s1, Signer.ids(3)));
        assertEq(v.status(), B3TypesV2.STATUS_LIVE);
        assertEq(v.nextSetHash(), s1.setHash);
    }

    function test_Deploy_HonoursExplicitOptionalValues() public {
        DeployV2.Params memory p = script.readParams();
        p.forwarderFactory = FD_FACTORY;
        p.tokenCodeHash = address(usdt).codehash;
        p.windows = _b10Windows(); // B-10 orderings hold on 31337 too
        p.watchers = new address[](0);
        p.maxDepositRaw = 1_000 * UNIT;
        p.minBridgeValidators = 5;
        p.maxBridgeValidators = 32;
        p.minBridgeTotalWeight = 1_000;
        DeployV2.Deployment memory d = script.deploy(p);

        assertFalse(d.forwarderFactoryPredicted);
        assertEq(d.vault.FORWARDER_FACTORY(), FD_FACTORY);
        assertEq(d.vault.MAX_DEPOSIT_RAW(), 1_000 * UNIT);
        assertEq(d.verifier.MIN_EPOCH_DURATION(), B10_MIN_EPOCH);
        assertEq(d.verifier.MAX_EPOCH_LAG(), B10_LAG);
        assertEq(d.verifier.DEPOSIT_CLOSE_AFTER(), B10_CLOSE);
        assertEq(d.verifier.MAX_CERTIFICATE_AGE(), B10_AGE);
        assertEq(d.verifier.REVIEW_WINDOW(), B10_REVIEW);
        assertEq(d.verifier.INIT_DEADLINE(), T0 + B10_INIT_WINDOW);
        assertEq(uint256(d.verifier.MIN_BRIDGE_VALIDATORS()), 5);
        assertEq(uint256(d.verifier.MAX_BRIDGE_VALIDATORS()), 32);
        assertEq(uint256(d.verifier.MIN_BRIDGE_TOTAL_WEIGHT()), 1_000);
        assertEq(d.verifier.watchers().length, 0);
        assertEq(d.verifier.WATCHER_COUNT(), 0);
    }

    function test_Deploy_PreBroadcastChecks() public {
        DeployV2.Params memory p;

        p = script.readParams();
        p.deployer = address(0);
        _expectDeployRevert(p, abi.encodeWithSelector(DeployV2.BadParam.selector, "B3V2_DEPLOYER"));

        p = script.readParams();
        p.originToken = address(0xC0DE1E55);
        _expectDeployRevert(p, abi.encodeWithSelector(DeployV2.BadParam.selector, "B3V2_ORIGIN_TOKEN"));

        p = script.readParams();
        p.lineageId = keccak256("another lineage");
        _expectDeployRevert(p, abi.encodeWithSelector(DeployV2.LineageMismatch.selector, lineageIdV, p.lineageId));

        p = script.readParams();
        p.genesisHeight = G + 1;
        bytes32 moved = B3TypesV2.lineageId(DOMAIN, G + 1, genesisHash(), genesisSetHash);
        _expectDeployRevert(p, abi.encodeWithSelector(DeployV2.LineageMismatch.selector, moved, lineageIdV));

        p = script.readParams();
        p.windows.depositCloseAfter = p.windows.maxEpochLag;
        _expectDeployRevert(
            p, abi.encodeWithSelector(DeployV2.BadWindows.selector, "need DEPOSIT_CLOSE_AFTER < MAX_EPOCH_LAG")
        );

        p = script.readParams();
        p.bootstrapAggregatePubkey = new bytes(47);
        _expectDeployRevert(p, abi.encodeWithSelector(DeployV2.BadParam.selector, "B3V2_BOOTSTRAP_AGGREGATE_PUBKEY"));

        p = script.readParams();
        p.bootstrapMembersRoot = bytes32(0);
        _expectDeployRevert(p, abi.encodeWithSelector(DeployV2.BadParam.selector, "B3V2_BOOTSTRAP_MEMBERS_ROOT"));

        p = script.readParams();
        p.bootstrapSetHash = keccak256("another bootstrap set");
        _expectDeployRevert(p, abi.encodeWithSelector(DeployV2.BadParam.selector, "B3V2_BOOTSTRAP_SET_HASH"));

        p = script.readParams();
        p.bootstrapSetHash = bytes32(0);
        _expectDeployRevert(p, abi.encodeWithSelector(DeployV2.BadParam.selector, "B3V2_BOOTSTRAP_SET_HASH"));

        p = script.readParams();
        p.bootstrapAggregatePubkey[0] = bytes1(uint8(p.bootstrapAggregatePubkey[0]) | 0x40); // infinity flag
        p.bootstrapSetHash =
            B3Types.hashSetHeader(script.bootstrapHeader(p.bootstrapAggregatePubkey, p.bootstrapMembersRoot));
        _expectDeployRevert(p, abi.encodeWithSelector(DeployV2.BadParam.selector, "B3V2_BOOTSTRAP_SET_HASH"));

        p = script.readParams();
        p.bootstrapMode = START_CERT; // keeps the K_OF_N set hash
        _expectDeployRevert(p, abi.encodeWithSelector(DeployV2.BadParam.selector, "B3V2_BOOTSTRAP_SET_HASH"));

        p = script.readParams();
        p.tokenCodeHash = keccak256("another token runtime");
        _expectDeployRevert(p, abi.encodeWithSelector(DeployV2.BadParam.selector, "B3V2_TOKEN_CODE_HASH"));

        assertEq(uint256(CHEATS.getNonce(DEPLOYER)), 0, "nothing deployed");
    }

    /// Constructor guards the script does not repeat still stop the deployment.
    function test_Deploy_VerifierConstructorGuardStops() public {
        DeployV2.Params memory p = script.readParams();
        p.startSetHash = lineageSet(1).setHash; // epoch 0 must start at GENESIS_SET_HASH
        vm.expectRevert(B3FinalityVerifierV2.BadConfig.selector);
        script.deploy(p);
        assertEq(uint256(CHEATS.getNonce(DEPLOYER)), 0, "nothing deployed");
    }

    function test_Deploy_WatcherGuardStops() public {
        DeployV2.Params memory p = script.readParams();
        p.watchers = new address[](6);
        for (uint256 i = 0; i < 6; ++i) {
            p.watchers[i] = address(uint160(0xA000 + i));
        }
        vm.expectRevert(B3FinalityVerifierV2.BadWatchers.selector);
        script.deploy(p);
    }

    function test_Deploy_VaultConstructorGuardStops() public {
        DeployV2.Params memory p = script.readParams();
        p.maxDepositRaw = 0;
        vm.expectRevert(B3VaultV2.BadConfig.selector);
        script.deploy(p);
        assertEq(uint256(CHEATS.getNonce(DEPLOYER)), 0, "nothing deployed");
    }

    // ============================================================= post-check

    function test_VerifyDeployment_CatchesEveryMismatch() public {
        DeployV2.Params memory p = script.readParams();
        DeployV2.Deployment memory d = script.deploy(p);
        script.verifyDeployment(p, d);

        p.windows.minEpochDuration += 1;
        _expectPostCheck(p, d, "MIN_EPOCH_DURATION");
        p.windows.minEpochDuration -= 1;

        p.chainDomain = keccak256("another domain");
        _expectPostCheck(p, d, "CHAIN_DOMAIN");
        p.chainDomain = DOMAIN;

        p.lineageId = keccak256("another lineage");
        _expectPostCheck(p, d, "LINEAGE_ID");
        p.lineageId = lineageIdV;

        p.startSetHash = keccak256("another start set");
        _expectPostCheck(p, d, "START_SET_HASH");
        p.startSetHash = genesisSetHash;

        p.bridgeActivationHeight += 1;
        _expectPostCheck(p, d, "BRIDGE_ACTIVATION_HEIGHT");
        p.bridgeActivationHeight -= 1;

        address[] memory oneWatcher = new address[](1);
        oneWatcher[0] = WATCHER_A;
        address[] memory twoWatchers = p.watchers;
        p.watchers = oneWatcher;
        _expectPostCheck(p, d, "WATCHERS");
        p.watchers = twoWatchers;

        d.initDeadline += 1;
        _expectPostCheck(p, d, "INIT_DEADLINE");
        d.initDeadline -= 1;

        p.maxDepositRaw += 1;
        _expectPostCheck(p, d, "MAX_DEPOSIT_RAW");
        p.maxDepositRaw -= 1;

        p.originToken = USDT_MAINNET;
        _expectPostCheck(p, d, "ORIGIN_TOKEN");
        p.originToken = address(usdt);

        bytes32 assetId = d.assetId;
        d.assetId = keccak256("another asset");
        _expectPostCheck(p, d, "B3_ASSET_ID");
        d.assetId = assetId;

        d.deployerNonce = 1;
        _expectPostCheck(p, d, "prover address");
        d.deployerNonce = 0;

        address factory = d.forwarderFactory;
        d.forwarderFactory = FD_FACTORY;
        _expectPostCheck(p, d, "predicted forwarder factory");
        d.forwarderFactory = factory;

        bytes32 vaultCodeHash = d.vaultCodeHash;
        d.vaultCodeHash = keccak256("another vault runtime");
        _expectPostCheck(p, d, "vault code");
        d.vaultCodeHash = vaultCodeHash;

        script.verifyDeployment(p, d);
    }

    // ================================================================ windows

    function test_ValidateWindows_ProfileROnlyOnRegtest() public {
        DeployV2.Windows memory r = _profileRWindows();
        DeployV2.Windows memory b10 = _b10Windows();
        script.validateWindows(31_337, r);
        script.validateWindows(31_337, b10);

        uint256[4] memory other = [uint256(1), 11_155_111, 42_161, 0];
        for (uint256 i = 0; i < other.length; ++i) {
            vm.expectRevert(abi.encodeWithSelector(DeployV2.BadWindows.selector, "need MIN_EPOCH_DURATION >= 1 h"));
            script.validateWindows(other[i], r);
            script.validateWindows(other[i], b10);
        }

        // Profile R with a 1 h minimum still fails the 7-day lag margin off 31337.
        r.minEpochDuration = 3_600;
        script.validateWindows(31_337, r);
        vm.expectRevert(
            abi.encodeWithSelector(DeployV2.BadWindows.selector, "need MAX_EPOCH_LAG >= DEPOSIT_CLOSE_AFTER + 7 days")
        );
        script.validateWindows(1, r);
        vm.expectRevert(
            abi.encodeWithSelector(DeployV2.BadWindows.selector, "need MAX_EPOCH_LAG >= DEPOSIT_CLOSE_AFTER + 7 days")
        );
        script.validateWindows(11_155_111, r);

        // The verifier constructor agrees: profile R deploys on 31337 only.
        deployWith(config(B3FinalityVerifierV2.BootstrapMode.K_OF_N, 0, genesisSetHash, profileR()));
        vm.chainId(1);
        B3FinalityVerifierV2.Config memory c =
            config(B3FinalityVerifierV2.BootstrapMode.K_OF_N, 0, genesisSetHash, profileR());
        vm.expectRevert(B3FinalityVerifierV2.BadWindows.selector);
        new B3FinalityVerifierV2(c, bootstrapHeader, defaultWatchers());
    }

    function test_ValidateWindows_MainnetBoundaries() public {
        DeployV2.Windows memory w = _b10Windows();
        w.minEpochDuration = 3_600;
        script.validateWindows(1, w);
        w.minEpochDuration = 3_599;
        vm.expectRevert(abi.encodeWithSelector(DeployV2.BadWindows.selector, "need MIN_EPOCH_DURATION >= 1 h"));
        script.validateWindows(1, w);

        w = _b10Windows();
        w.maxEpochLag = w.depositCloseAfter + 7 days;
        script.validateWindows(1, w);
        w.maxEpochLag -= 1;
        vm.expectRevert(
            abi.encodeWithSelector(DeployV2.BadWindows.selector, "need MAX_EPOCH_LAG >= DEPOSIT_CLOSE_AFTER + 7 days")
        );
        script.validateWindows(1, w);

        // The 7-day review floor off regtest, as in the verifier constructor.
        w = _b10Windows();
        w.reviewWindow = 7 days;
        script.validateWindows(1, w);
        w.reviewWindow = 7 days - 1;
        vm.expectRevert(abi.encodeWithSelector(DeployV2.BadWindows.selector, "need REVIEW_WINDOW >= 7 days"));
        script.validateWindows(1, w);
        script.validateWindows(31_337, w);
    }

    function test_ValidateWindows_Orderings() public {
        DeployV2.Windows memory w;

        w = _profileRWindows();
        w.maxEpochLag = 0;
        _expectWindows(w, "MAX_EPOCH_LAG out of range");
        w = _profileRWindows();
        w.maxEpochLag = uint256(type(uint40).max) + 1;
        _expectWindows(w, "MAX_EPOCH_LAG out of range");

        w = _profileRWindows();
        w.maxCertificateAge = 0;
        _expectWindows(w, "need 0 < MAX_CERTIFICATE_AGE < DEPOSIT_CLOSE_AFTER");
        w = _profileRWindows();
        w.maxCertificateAge = w.depositCloseAfter;
        _expectWindows(w, "need 0 < MAX_CERTIFICATE_AGE < DEPOSIT_CLOSE_AFTER");

        w = _profileRWindows();
        w.minEpochDuration = 0;
        _expectWindows(w, "need 0 < MIN_EPOCH_DURATION <= MAX_EPOCH_LAG");
        w = _profileRWindows();
        w.minEpochDuration = w.maxEpochLag + 1;
        _expectWindows(w, "need 0 < MIN_EPOCH_DURATION <= MAX_EPOCH_LAG");
        w.minEpochDuration = w.maxEpochLag;
        script.validateWindows(31_337, w);

        w = _profileRWindows();
        w.reviewWindow = 0;
        _expectWindows(w, "need 0 < REVIEW_WINDOW < MAX_EPOCH_LAG");
        w = _profileRWindows();
        w.reviewWindow = w.maxEpochLag;
        _expectWindows(w, "need 0 < REVIEW_WINDOW < MAX_EPOCH_LAG");

        w = _profileRWindows();
        w.initWindow = 0;
        _expectWindows(w, "need 0 < INIT_WINDOW <= MAX_EPOCH_LAG");
        w = _profileRWindows();
        w.initWindow = w.maxEpochLag + 1;
        _expectWindows(w, "need 0 < INIT_WINDOW <= MAX_EPOCH_LAG");
        w.initWindow = w.maxEpochLag;
        script.validateWindows(31_337, w);

        w = _profileRWindows();
        w.depositCloseAfter = w.maxEpochLag;
        _expectWindows(w, "need DEPOSIT_CLOSE_AFTER < MAX_EPOCH_LAG");
        w.depositCloseAfter = w.maxEpochLag - 1;
        script.validateWindows(31_337, w);
    }

    // ========================================================== mainnet pins

    function test_ValidateMainnetPins_AcceptsB10() public view {
        script.validateMainnetPins(_mainnetPins());
        DeployV2.MainnetPins memory m = _mainnetPins();
        // 259,200 >= 1,472 x 161 + 21,600 = 258,592: 72 h holds up to ~161 s blocks.
        m.sMax = 161;
        script.validateMainnetPins(m);
        m.sMax = 1;
        script.validateMainnetPins(m);
        m.initDeadline = m.nowTimestamp + 1;
        script.validateMainnetPins(m);
    }

    function test_ValidateMainnetPins_RejectsEachDeviation() public {
        DeployV2.MainnetPins memory m;

        m = _mainnetPins();
        m.chainId = 11_155_111;
        _expectPin(m, "chain id");
        m = _mainnetPins();
        m.chainId = 31_337;
        _expectPin(m, "chain id");

        m = _mainnetPins();
        m.originToken = address(usdt);
        _expectPin(m, "origin token");

        m = _mainnetPins();
        m.minBridgeValidators = 3;
        _expectPin(m, "MIN_BRIDGE_VALIDATORS");
        m = _mainnetPins();
        m.minBridgeValidators = 5;
        _expectPin(m, "MIN_BRIDGE_VALIDATORS");

        m = _mainnetPins();
        m.maxBridgeValidators = 63;
        _expectPin(m, "MAX_BRIDGE_VALIDATORS");
        m = _mainnetPins();
        m.maxBridgeValidators = 65;
        _expectPin(m, "MAX_BRIDGE_VALIDATORS");

        m = _mainnetPins();
        m.minBridgeTotalWeight = 899;
        _expectPin(m, "MIN_BRIDGE_TOTAL_WEIGHT");
        m = _mainnetPins();
        m.minBridgeTotalWeight = 901;
        _expectPin(m, "MIN_BRIDGE_TOTAL_WEIGHT");

        m = _mainnetPins();
        m.maxDepositRaw = CAP - 1;
        _expectPin(m, "MAX_DEPOSIT_RAW");
        m = _mainnetPins();
        m.maxDepositRaw = CAP + 1;
        _expectPin(m, "MAX_DEPOSIT_RAW");

        m = _mainnetPins();
        m.minEpochDuration = B10_MIN_EPOCH - 1;
        _expectPin(m, "MIN_EPOCH_DURATION");
        m = _mainnetPins();
        m.minEpochDuration = B10_MIN_EPOCH + 1;
        _expectPin(m, "MIN_EPOCH_DURATION");

        m = _mainnetPins();
        m.maxEpochLag = B10_LAG - 1;
        _expectPin(m, "MAX_EPOCH_LAG");
        m = _mainnetPins();
        m.maxEpochLag = B10_LAG + 1;
        _expectPin(m, "MAX_EPOCH_LAG");

        m = _mainnetPins();
        m.depositCloseAfter = B10_CLOSE - 1;
        _expectPin(m, "DEPOSIT_CLOSE_AFTER");
        m = _mainnetPins();
        m.depositCloseAfter = B10_CLOSE + 1;
        _expectPin(m, "DEPOSIT_CLOSE_AFTER");

        m = _mainnetPins();
        m.maxCertificateAge = B10_AGE - 1;
        _expectPin(m, "MAX_CERTIFICATE_AGE");
        m = _mainnetPins();
        m.maxCertificateAge = B10_AGE + 1;
        _expectPin(m, "MAX_CERTIFICATE_AGE");

        m = _mainnetPins();
        m.reviewWindow = 7 days - 1;
        _expectPin(m, "REVIEW_WINDOW");
        m = _mainnetPins();
        m.reviewWindow = 7 days + 1;
        _expectPin(m, "REVIEW_WINDOW");

        m = _mainnetPins();
        m.initDeadline = m.nowTimestamp;
        _expectPin(m, "INIT_DEADLINE");
        m = _mainnetPins();
        m.initDeadline = m.nowTimestamp + 7 days + 1;
        _expectPin(m, "INIT_DEADLINE");

        m = _mainnetPins();
        m.sMax = 0;
        _expectPin(m, "S_MAX");
        m = _mainnetPins();
        m.sMax = 162; // 1,472 x 162 + 21,600 = 260,064 > 259,200
        _expectPin(m, "S_MAX");
        m = _mainnetPins();
        m.sMax = type(uint256).max;
        _expectPin(m, "S_MAX");
    }

    // =============================================================== manifest

    function test_ManifestPath_OnlyGitignoredLocalFiles() public {
        script.validateManifestPath("deployments/local-rt4.json");
        script.validateManifestPath("deployments/local-rt4.v2.json");

        string[11] memory refused = [
            "",
            "deployments/local-.json",
            "deployments/rt4.json",
            "deployments/local-rt4.txt",
            "deployments/local-rt4.json.bak",
            "deployments/local-a/b.json",
            "deployments/local-..\\x.json",
            "deployments/local-../ethereum-mainnet-v1.1.1.json",
            "./deployments/local-rt4.json",
            "/tmp/deployments/local-rt4.json",
            "Deployments/local-rt4.json"
        ];
        for (uint256 i = 0; i < refused.length; ++i) {
            vm.expectRevert(abi.encodeWithSelector(DeployV2.BadManifestPath.selector, refused[i]));
            script.validateManifestPath(refused[i]);
        }
    }

    function test_ManifestJson_CarriesEveryPin() public {
        DeployV2.Params memory p = script.readParams();
        DeployV2.Deployment memory d = script.deploy(p);
        string memory json = script.manifestJson(p, d);

        assertEq(vm.parseJsonString(json, ".schema"), "b3-bridge-v2-deployment/1");
        assertEq(vm.parseJsonString(json, ".status"), "regtest_candidate");
        assertFalse(vm.parseJsonBool(json, ".production_approved"));
        assertEq(vm.parseJsonUint(json, ".chain_id"), 31_337);
        assertEq(vm.parseJsonAddress(json, ".deployer"), DEPLOYER);
        assertEq(vm.parseJsonUint(json, ".deployer_nonce"), 0);

        assertEq(vm.parseJsonAddress(json, ".prover"), address(d.prover));
        assertEq(vm.parseJsonBytes32(json, ".prover_code_hash"), address(d.prover).codehash);
        assertEq(vm.parseJsonAddress(json, ".verifier"), address(d.verifier));
        assertEq(vm.parseJsonBytes32(json, ".verifier_code_hash"), address(d.verifier).codehash);
        assertEq(vm.parseJsonAddress(json, ".vault"), address(d.vault));
        assertEq(vm.parseJsonBytes32(json, ".vault_code_hash"), address(d.vault).codehash);
        assertEq(vm.parseJsonAddress(json, ".relay_batch"), address(d.batch));
        assertEq(vm.parseJsonBytes32(json, ".relay_batch_code_hash"), address(d.batch).codehash);
        assertEq(vm.parseJsonBytes32(json, ".b3_asset_id"), d.vault.B3_ASSET_ID());

        assertEq(vm.parseJsonBytes32(json, ".chain_domain"), DOMAIN);
        assertEq(vm.parseJsonUint(json, ".genesis_height"), G);
        assertEq(vm.parseJsonBytes32(json, ".genesis_block_hash"), genesisHash());
        assertEq(vm.parseJsonBytes32(json, ".genesis_set_hash"), genesisSetHash);
        assertEq(vm.parseJsonBytes32(json, ".lineage_id"), lineageIdV);
        assertEq(vm.parseJsonUint(json, ".start_epoch"), 0);
        assertEq(vm.parseJsonBytes32(json, ".start_set_hash"), genesisSetHash);
        assertEq(vm.parseJsonUint(json, ".bootstrap_mode"), 0);
        assertEq(vm.parseJsonString(json, ".bootstrap_mode_name"), "K_OF_N");
        assertEq(vm.parseJsonBytes32(json, ".bootstrap_set_hash"), bootstrapSetHash);
        assertEq(vm.parseJsonBytes(json, ".bootstrap_aggregate_pubkey"), bootstrapHeader.aggregatePubkey);
        assertEq(vm.parseJsonBytes32(json, ".bootstrap_members_root"), bootstrapHeader.membersRoot);
        assertEq(vm.parseJsonUint(json, ".bridge_activation_height"), V);
        assertEq(vm.parseJsonUint(json, ".min_bridge_validators"), 4);
        assertEq(vm.parseJsonUint(json, ".max_bridge_validators"), 64);
        assertEq(vm.parseJsonUint(json, ".min_bridge_total_weight"), 900);

        assertEq(vm.parseJsonUint(json, ".min_epoch_duration"), MIN_EPOCH);
        assertEq(vm.parseJsonUint(json, ".max_epoch_lag"), LAG);
        assertEq(vm.parseJsonUint(json, ".deposit_close_after"), CLOSE);
        assertEq(vm.parseJsonUint(json, ".max_certificate_age"), AGE);
        assertEq(vm.parseJsonUint(json, ".init_window"), INIT_WINDOW);
        assertEq(vm.parseJsonUint(json, ".init_deadline"), T0 + INIT_WINDOW);
        assertEq(vm.parseJsonUint(json, ".review_window"), REVIEW);
        address[] memory watchers = CHEATS.parseJsonAddressArray(json, ".watchers");
        assertEq(watchers.length, 2);
        assertEq(watchers[0], WATCHER_A);
        assertEq(watchers[1], WATCHER_B);

        assertEq(vm.parseJsonAddress(json, ".origin_token"), address(usdt));
        assertEq(vm.parseJsonBytes32(json, ".token_code_hash"), address(usdt).codehash);
        assertEq(vm.parseJsonUint(json, ".max_deposit_raw"), CAP);
        assertEq(vm.parseJsonAddress(json, ".predecessor"), address(0));
        assertEq(vm.parseJsonAddress(json, ".forwarder_factory"), CHEATS.computeCreateAddress(DEPLOYER, 4));
        assertEq(vm.parseJsonUint(json, ".forwarder_factory_create_nonce"), 4);
        assertTrue(vm.parseJsonBool(json, ".forwarder_factory_predicted"));
        // Codeless until the F_d slice: a latent deployer privilege (nonce n+4).
        assertEq(vm.parseJsonBytes32(json, ".forwarder_factory_code_hash"), bytes32(0));
        assertTrue(vm.parseJsonBool(json, ".forwarder_factory_latent_deployer_privilege"));

        // The harness takes the deployment block from receipts or the broadcast output.
        assertFalse(vm.keyExistsJson(json, ".deployment_block"));

        // A factory with code is recorded by its code hash, with no latent privilege.
        p = script.readParams();
        p.forwarderFactory = address(usdt);
        d = script.deploy(p);
        json = script.manifestJson(p, d);
        assertFalse(vm.parseJsonBool(json, ".forwarder_factory_predicted"));
        assertEq(vm.parseJsonBytes32(json, ".forwarder_factory_code_hash"), address(usdt).codehash);
        assertFalse(vm.parseJsonBool(json, ".forwarder_factory_latent_deployer_privilege"));
    }

    // ================================================================ helpers

    function _expectDeployRevert(DeployV2.Params memory p, bytes memory reason) private {
        vm.expectRevert(reason);
        script.deploy(p);
    }

    function _expectPostCheck(DeployV2.Params memory p, DeployV2.Deployment memory d, string memory what) private {
        vm.expectRevert(abi.encodeWithSelector(DeployV2.PostCheckFailed.selector, what));
        script.verifyDeployment(p, d);
    }

    function _expectWindows(DeployV2.Windows memory w, string memory reason) private {
        vm.expectRevert(abi.encodeWithSelector(DeployV2.BadWindows.selector, reason));
        script.validateWindows(31_337, w);
    }

    function _expectPin(DeployV2.MainnetPins memory m, string memory name) private {
        vm.expectRevert(abi.encodeWithSelector(DeployV2.BadMainnetPin.selector, name));
        script.validateMainnetPins(m);
    }

    function _profileRWindows() private pure returns (DeployV2.Windows memory) {
        return DeployV2.Windows({
            minEpochDuration: MIN_EPOCH,
            maxEpochLag: LAG,
            depositCloseAfter: CLOSE,
            maxCertificateAge: AGE,
            initWindow: INIT_WINDOW,
            reviewWindow: REVIEW
        });
    }

    function _b10Windows() private pure returns (DeployV2.Windows memory) {
        return DeployV2.Windows({
            minEpochDuration: B10_MIN_EPOCH,
            maxEpochLag: B10_LAG,
            depositCloseAfter: B10_CLOSE,
            maxCertificateAge: B10_AGE,
            initWindow: B10_INIT_WINDOW,
            reviewWindow: B10_REVIEW
        });
    }

    /// The chain-1 B-10 set: 21,600 / 2,592,000 / 259,200 / 86,400 s,
    /// 4 / 64 / 900, 10,000 USDT, mainnet USDT, 7-day review and init window,
    /// and S_MAX at the computed ~78 s mean spacing (O8 unmeasured).
    function _mainnetPins() private pure returns (DeployV2.MainnetPins memory) {
        return DeployV2.MainnetPins({
            chainId: 1,
            originToken: USDT_MAINNET,
            minBridgeValidators: 4,
            maxBridgeValidators: 64,
            minBridgeTotalWeight: 900,
            maxDepositRaw: CAP,
            minEpochDuration: B10_MIN_EPOCH,
            maxEpochLag: B10_LAG,
            depositCloseAfter: B10_CLOSE,
            maxCertificateAge: B10_AGE,
            reviewWindow: B10_REVIEW,
            initDeadline: NOW_MAINNET + 7 days,
            nowTimestamp: NOW_MAINNET,
            sMax: 78
        });
    }
}
