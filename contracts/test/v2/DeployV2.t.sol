// SPDX-License-Identifier: MIT
pragma solidity ^0.8.28;

import {VmV2} from "./V2TestBase.sol";
import {RelayFixtureV2} from "./utils/RelayFixtureV2.sol";
import {LzPacketsV2} from "./utils/ForwarderFixtureV2.sol";
import {BlsTestSignerV2 as Signer} from "./utils/BlsTestSignerV2.sol";
import {B3Types} from "../../src/IB3FinalityProver.sol";
import {B3TypesV2} from "../../src/v2/B3TypesV2.sol";
import {B3FinalityVerifierV2} from "../../src/v2/B3FinalityVerifierV2.sol";
import {B3VaultV2} from "../../src/v2/B3VaultV2.sol";
import {B3RelayBatch} from "../../src/v2/B3RelayBatch.sol";
import {B3Forwarder} from "../../src/v2/B3Forwarder.sol";
import {B3ForwarderTypesV2} from "../../src/v2/B3ForwarderTypesV2.sol";
import {B3DepositForwarderFactory} from "../../src/v2/B3DepositForwarderFactory.sol";
import {B3OftExitFactory} from "../../src/v2/B3OftExitFactory.sol";
import {IOFTV2} from "../../src/v2/IOFTV2.sol";
import {MockTetherToken} from "../../regtest/MockTetherToken.sol";
import {MockLzEndpoint} from "../../regtest/MockLzEndpoint.sol";
import {MockOFTAdapter} from "../../regtest/MockOFTAdapter.sol";
import {MockUSDT0} from "../../regtest/MockUSDT0.sol";
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
///
/// Forwarder factories (B3V2_DEPLOY_FORWARDERS; contracts spec §7-§8) are
/// tested through deploy(Params) only: setting the flag in the shared
/// environment would turn every parallel run() into a six-CREATE deployment.
/// One in-process EVM stands for both anvils (31337 and 31338), with the
/// regtest LayerZero mocks and LzPacketsV2 as the harness relayer.
contract DeployV2Test is RelayFixtureV2 {
    using LzPacketsV2 for LzPacketsV2.Packet;

    DeployCheatsV2 private constant CHEATS = DeployCheatsV2(address(uint160(uint256(keccak256("hevm cheat code")))));

    address private constant DEPLOYER = address(0xDE9107E2);
    address private constant USDT_MAINNET = 0xdAC17F958D2ee523a2206206994597C13D831ec7;
    uint256 private constant NOW_MAINNET = 1_800_000_000;
    B3FinalityVerifierV2.BootstrapMode private constant START_CERT =
    B3FinalityVerifierV2.BootstrapMode.START_CERTIFICATE;

    uint32 private constant ETH_EID = 30101;
    uint32 private constant ARB_EID = 30110;
    /// Opaque executor options F_x passes through (a type-3 lzReceive gas option).
    bytes private constant EXTRA_OPTIONS = hex"00030100110100000000000000000000000000030d40";
    /// Relayer fee per flush and per forward (1 USDT).
    uint256 private constant FWD_FEE = 1_000_000;
    /// F_d lifetime fee cap and F_x per-forward fee cap.
    uint96 private constant DEPOSIT_FEE_CAP = 4_000_000;
    uint96 private constant EXIT_FEE_CAP = 5_000_000;
    /// Per-item gas cap for forwards: above a first forward (deploys F_x).
    uint256 private constant FORWARD_GAS = 600_000;
    address private constant RELAYER = address(0x5E1A7E01);
    address private constant FEE_SINK = address(0xFEE51C);
    address private constant REFUND_TO = address(0x7EF0D0);
    address private constant ARB_USER = address(0xA4B1);

    /// The LayerZero mocks: endpoint 30101 with MockOFTAdapter over the
    /// fixture's USDT (Ethereum), endpoint 30110 with MockUSDT0 (Arbitrum),
    /// peered.
    struct LzMocks {
        MockLzEndpoint endpointEth;
        MockLzEndpoint endpointArb;
        MockOFTAdapter adapter;
        MockUSDT0 usdt0;
    }

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
            v, initArgs(v, 1, bootstrapKeys(), s0, s1, Signer.ids(3)), B3FinalityVerifierV2.BadSigningSet.selector
        );
        doInit(v, initArgs(v, 1, s0, s0, s1, Signer.ids(3)));
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

    // ============================================================= forwarders

    function test_ReadParams_ForwardersOffByDefault() public view {
        DeployV2.Params memory p = script.readParams();
        assertFalse(p.deployForwarders);
        assertEq(p.oftAdapter, address(0));
        assertEq(uint256(p.exitDstEid), ARB_EID, "Arbitrum One by default");
        assertEq(uint256(script.DEFAULT_EXIT_DST_EID()), ARB_EID);
        assertEq(p.exitExtraOptions.length, 0);
    }

    function test_ParseDeployForwarders() public {
        assertFalse(script.parseDeployForwarders(0));
        assertTrue(script.parseDeployForwarders(1));
        vm.expectRevert(abi.encodeWithSelector(DeployV2.BadParam.selector, "B3V2_DEPLOY_FORWARDERS"));
        script.parseDeployForwarders(2);
        vm.expectRevert(abi.encodeWithSelector(DeployV2.BadParam.selector, "B3V2_DEPLOY_FORWARDERS"));
        script.parseDeployForwarders(type(uint256).max);
    }

    /// The default keeps the four CREATEs: nonce 4 (the F_d slot) and nonce 5
    /// (RT4's OTHER_VERIFIER_NONCE) stay codeless, `d` names no forwarder
    /// factory, and the manifest's forwarder keys are present and zero.
    function test_Deploy_DefaultLeavesForwarderSlotsEmpty() public {
        DeployV2.Params memory p = script.readParams();
        DeployV2.Deployment memory d = script.deploy(p);
        assertEq(uint256(CHEATS.getNonce(DEPLOYER)), 4, "exactly four deployments");
        assertEq(CHEATS.computeCreateAddress(DEPLOYER, 4).code.length, 0);
        assertEq(CHEATS.computeCreateAddress(DEPLOYER, 5).code.length, 0);
        assertEq(address(d.depositFactory), address(0));
        assertEq(d.depositFactoryCodeHash, bytes32(0));
        assertEq(address(d.exitFactory), address(0));
        assertEq(d.exitFactoryCodeHash, bytes32(0));
        assertEq(d.forwarderInitCodeHash, bytes32(0));

        string memory json = script.manifestJson(p, d);
        assertFalse(vm.parseJsonBool(json, ".deploy_forwarders"));
        assertEq(vm.parseJsonAddress(json, ".exit_forwarder_factory"), address(0));
        assertEq(vm.parseJsonBytes32(json, ".exit_forwarder_factory_code_hash"), bytes32(0));
        assertEq(vm.parseJsonBytes32(json, ".forwarder_init_code_hash"), bytes32(0));
        assertEq(vm.parseJsonAddress(json, ".oft_adapter"), address(0));
        assertEq(vm.parseJsonUint(json, ".exit_dst_eid"), 0);
        assertTrue(vm.keyExistsJson(json, ".exit_extra_options"));
        assertEq(vm.parseJsonBytes(json, ".exit_extra_options").length, 0);
        assertEq(vm.parseJsonBytes32(json, ".forwarder_factory_code_hash"), bytes32(0));
        assertTrue(vm.parseJsonBool(json, ".forwarder_factory_latent_deployer_privilege"));
    }

    /// B3V2_DEPLOY_FORWARDERS: the same broadcast adds the F_d factory at
    /// nonce 4, the vault's predicted FORWARDER_FACTORY, and the F_x factory
    /// at nonce 5, each with its pinned immutables and the shared B3Forwarder
    /// init code hash; the four core contracts keep their nonces and code.
    function test_Deploy_WithForwarders_AddressesAndImmutables() public {
        LzMocks memory m = _lzMocks();
        DeployV2.Params memory p = _forwarderParams(address(m.adapter));
        DeployV2.Deployment memory d = script.deploy(p);

        assertEq(uint256(CHEATS.getNonce(DEPLOYER)), 6, "six deployments");
        assertEq(address(d.prover), CHEATS.computeCreateAddress(DEPLOYER, 0));
        assertEq(address(d.verifier), CHEATS.computeCreateAddress(DEPLOYER, 1));
        assertEq(address(d.vault), CHEATS.computeCreateAddress(DEPLOYER, 2));
        assertEq(address(d.batch), CHEATS.computeCreateAddress(DEPLOYER, 3));
        assertTrue(d.forwarderFactoryPredicted);
        assertEq(address(d.depositFactory), CHEATS.computeCreateAddress(DEPLOYER, 4));
        assertEq(address(d.depositFactory), d.forwarderFactory);
        assertEq(d.vault.FORWARDER_FACTORY(), address(d.depositFactory), "the vault's predicted factory is F_d");
        assertEq(address(d.exitFactory), CHEATS.computeCreateAddress(DEPLOYER, 5));
        assertEq(d.depositFactoryCodeHash, address(d.depositFactory).codehash);
        assertEq(d.exitFactoryCodeHash, address(d.exitFactory).codehash);
        assertEq(d.proverCodeHash, address(prover).codehash, "BlsDigestProver runtime");
        assertEq(d.batchCodeHash, address(batch).codehash, "B3RelayBatch runtime");

        bytes32 initCodeHash = keccak256(type(B3Forwarder).creationCode);
        assertEq(d.forwarderInitCodeHash, initCodeHash);

        B3DepositForwarderFactory fd = d.depositFactory;
        assertEq(fd.VAULT(), address(d.vault));
        assertEq(fd.TOKEN(), address(usdt));
        assertEq(fd.MAX_DEPOSIT_RAW(), CAP);
        assertEq(fd.MIN_FLUSH_RAW(), 1_000_000);
        assertEq(fd.MIN_FLUSH_RAW(), script.FORWARDER_MIN_FLUSH_RAW());
        assertEq(fd.FORWARDER_INIT_CODE_HASH(), initCodeHash);

        B3OftExitFactory fx = d.exitFactory;
        assertEq(fx.TOKEN(), address(usdt));
        assertEq(fx.ADAPTER(), address(m.adapter));
        assertEq(uint256(fx.DST_EID()), ARB_EID);
        assertEq(fx.extraOptions(), EXTRA_OPTIONS);
        assertEq(fx.FORWARDER_INIT_CODE_HASH(), initCodeHash);

        script.verifyDeployment(p, d);
    }

    /// The scripted stack carries routes (b) and (d3) end to end. USDT0 sent
    /// from Arbitrum to a fresh F_d address is delivered by the harness
    /// relayer before F_d exists, then deployed and flushed into the vault
    /// as one Deposit, the vault admitting F_d through its nonce-4 factory.
    /// A B3 withdrawal naming F_x is released and forwarded to Arbitrum in
    /// one relayWithForwards call of the deployed batch, the F_x fee paying
    /// the relayer, and the delivery mints USDT0 once.
    function test_Deploy_WithForwarders_CarriesRoutesBAndD3() public {
        LzMocks memory m = _lzMocks();
        DeployV2.Deployment memory d = script.deploy(_forwarderParams(address(m.adapter)));
        B3FinalityVerifierV2 v = d.verifier;

        B3ForwarderTypesV2.ExitParams memory ep = B3ForwarderTypesV2.ExitParams({
            dstEid: ARB_EID, to: _word(BOB), maxBridgeFeeBps: 50, feeCap: EXIT_FEE_CAP, refundEOA: ALICE
        });
        address exitAccount = d.exitFactory.accountOf(ep);
        B3TypesV2.WithdrawalV2[] memory ws = new B3TypesV2.WithdrawalV2[](1);
        ws[0] = withdrawal(0, exitAccount, 400 * UNIT, V);
        bytes32[] memory leaves = treeOf(d.vault, ws);

        bootstrap(v, 1, lineageSet(1));
        vm.warp(T0 + REVIEW);
        submit(v, canonicalCert(V_INDEX, 1, lineageSet(1), lineageSet(2), rootOf(leaves, 1)));
        assertTrue(d.vault.depositGateOpen());

        // Route (b): Arbitrum -> undeployed F_d -> deploy -> flush (PR-1: Transfer and Deposit only).
        B3ForwarderTypesV2.DepositParams memory dp = B3ForwarderTypesV2.DepositParams({
            vault: address(d.vault),
            b3Recipient: B3_RECIPIENT,
            refundEOA: ALICE,
            feeCap: DEPOSIT_FEE_CAP,
            // forge-lint: disable-next-line(unsafe-typecast)
            notAfter: uint64(block.timestamp + 30 days)
        });
        address depositAccount = d.depositFactory.accountOf(dp);
        _bridgeFromArbitrum(m, ARB_USER, depositAccount, 500 * UNIT);
        assertEq(usdt.balanceOf(depositAccount), 500 * UNIT);
        assertEq(depositAccount.code.length, 0, "credited before deployment");
        d.depositFactory.deploy(dp);
        vm.recordLogs();
        vm.prank(RELAYER, RELAYER);
        assertEq(d.depositFactory.flush(dp, FWD_FEE), 500 * UNIT - FWD_FEE);
        VmV2.Log[] memory logs = vm.getRecordedLogs();
        assertEq(logs.length, 2, "token Transfer and vault Deposit only");
        assertEq(logs[0].emitter, address(usdt));
        assertEq(logs[1].emitter, address(d.vault));
        assertEq(logs[1].topics[0], DEPOSIT_TOPIC);
        assertEq(d.vault.locked(), 500 * UNIT - FWD_FEE);
        assertEq(d.depositFactory.owed(depositAccount, RELAYER), FWD_FEE);

        // Route (d3): release to F_x and forward it in one batch call.
        uint256 q = m.endpointEth.quote();
        B3RelayBatch.ExitForward[] memory forwards = new B3RelayBatch.ExitForward[](1);
        forwards[0] = B3RelayBatch.ExitForward({p: ep, fee: FWD_FEE, feeRecipient: FEE_SINK, nativeFee: q});
        B3RelayBatch.ReleaseItem[] memory releases = items(item(leaves, 1, ws[0]));
        B3RelayBatch.CertInput[] memory certs = noCerts();
        vm.deal(RELAYER, q);
        vm.recordLogs();
        vm.prank(RELAYER, RELAYER);
        (uint256 certsAccepted, uint256 releasesPaid, uint256 forwardsSent) = d.batch.relayWithForwards{value: q}(
            v,
            certs,
            CERT_GAS,
            address(d.vault),
            releases,
            RELEASE_GAS,
            address(d.exitFactory),
            forwards,
            FORWARD_GAS,
            REFUND_TO
        );
        LzPacketsV2.Packet memory pk = LzPacketsV2.only(vm.getRecordedLogs(), address(m.endpointEth));
        assertEq(certsAccepted, 0);
        assertEq(releasesPaid, 1);
        assertEq(forwardsSent, 1);
        assertTrue(d.vault.released(0));
        assertEq(d.vault.locked(), 100 * UNIT - FWD_FEE);
        assertEq(usdt.balanceOf(exitAccount), 0, "the forward empties F_x");
        assertEq(usdt.balanceOf(FEE_SINK), FWD_FEE, "the F_x fee pays the relayer");
        assertEq(usdt.balanceOf(address(d.batch)), 0);
        assertEq(address(d.batch).balance, 0);
        assertEq(RELAYER.balance, 0, "exactly the quote spent");

        assertEq(pk.to(), _word(BOB));
        assertEq(pk.amount(), 400 * UNIT - FWD_FEE);
        pk.deliver(m.endpointArb);
        assertEq(m.usdt0.balanceOf(BOB), 400 * UNIT - FWD_FEE);
    }

    /// The forwarder pre-broadcast checks: the adapter must exist and name
    /// the origin token, the F_d factory takes the predicted slot (an
    /// explicit B3V2_FORWARDER_FACTORY is refused, even the predicted
    /// address), the destination endpoint id is non-zero and the cap admits
    /// a minimum flush. Without forwarders, forwarder inputs are refused.
    function test_Deploy_ForwarderPreBroadcastChecks() public {
        LzMocks memory m = _lzMocks();
        DeployV2.Params memory p;
        bytes memory badAdapter = abi.encodeWithSelector(DeployV2.BadParam.selector, "B3V2_OFT_ADAPTER");

        p = _forwarderParams(address(0)); // B3V2_OFT_ADAPTER missing
        _expectDeployRevert(p, badAdapter);
        p = _forwarderParams(address(0xADA9732)); // codeless
        _expectDeployRevert(p, badAdapter);
        p = _forwarderParams(address(usdt)); // code, but no token()
        _expectDeployRevert(p, badAdapter);
        MockTetherToken otherToken = new MockTetherToken();
        p = _forwarderParams(address(new MockOFTAdapter(address(otherToken), address(m.endpointEth))));
        _expectDeployRevert(p, badAdapter); // token() is not the origin token

        p = _forwarderParams(address(m.adapter));
        p.forwarderFactory = FD_FACTORY;
        _expectDeployRevert(p, abi.encodeWithSelector(DeployV2.BadParam.selector, "B3V2_FORWARDER_FACTORY"));
        p.forwarderFactory = CHEATS.computeCreateAddress(DEPLOYER, 4);
        _expectDeployRevert(p, abi.encodeWithSelector(DeployV2.BadParam.selector, "B3V2_FORWARDER_FACTORY"));

        p = _forwarderParams(address(m.adapter));
        p.exitDstEid = 0;
        _expectDeployRevert(p, abi.encodeWithSelector(DeployV2.BadParam.selector, "B3V2_EXIT_DST_EID"));

        p = _forwarderParams(address(m.adapter));
        p.maxDepositRaw = script.FORWARDER_MIN_FLUSH_RAW() - 1;
        _expectDeployRevert(p, abi.encodeWithSelector(DeployV2.BadParam.selector, "B3V2_MAX_DEPOSIT_RAW"));

        // Without forwarders, an adapter or option bytes is a mixed-up environment.
        p = script.readParams();
        p.oftAdapter = address(m.adapter);
        _expectDeployRevert(p, badAdapter);
        p = script.readParams();
        p.exitExtraOptions = EXTRA_OPTIONS;
        _expectDeployRevert(p, abi.encodeWithSelector(DeployV2.BadParam.selector, "B3V2_EXIT_EXTRA_OPTIONS"));

        assertEq(uint256(CHEATS.getNonce(DEPLOYER)), 0, "nothing deployed");

        // The minimum cap itself is admitted.
        p = _forwarderParams(address(m.adapter));
        p.maxDepositRaw = script.FORWARDER_MIN_FLUSH_RAW();
        DeployV2.Deployment memory d = script.deploy(p);
        assertEq(d.depositFactory.MAX_DEPOSIT_RAW(), 1_000_000);
    }

    function test_ManifestJson_WithForwarders() public {
        LzMocks memory m = _lzMocks();
        DeployV2.Params memory p = _forwarderParams(address(m.adapter));
        DeployV2.Deployment memory d = script.deploy(p);
        string memory json = script.manifestJson(p, d);

        assertTrue(vm.parseJsonBool(json, ".deploy_forwarders"));
        assertEq(vm.parseJsonAddress(json, ".forwarder_factory"), CHEATS.computeCreateAddress(DEPLOYER, 4));
        assertEq(vm.parseJsonAddress(json, ".forwarder_factory"), address(d.depositFactory));
        assertEq(vm.parseJsonUint(json, ".forwarder_factory_create_nonce"), 4);
        assertTrue(vm.parseJsonBool(json, ".forwarder_factory_predicted"));
        bytes32 depositFactoryCodeHash = vm.parseJsonBytes32(json, ".forwarder_factory_code_hash");
        assertEq(depositFactoryCodeHash, address(d.depositFactory).codehash);
        assertNotEq(depositFactoryCodeHash, bytes32(0));
        assertFalse(vm.parseJsonBool(json, ".forwarder_factory_latent_deployer_privilege"), "no latent privilege");

        assertEq(vm.parseJsonAddress(json, ".exit_forwarder_factory"), CHEATS.computeCreateAddress(DEPLOYER, 5));
        assertEq(vm.parseJsonAddress(json, ".exit_forwarder_factory"), address(d.exitFactory));
        assertEq(vm.parseJsonBytes32(json, ".exit_forwarder_factory_code_hash"), address(d.exitFactory).codehash);
        assertEq(vm.parseJsonBytes32(json, ".forwarder_init_code_hash"), keccak256(type(B3Forwarder).creationCode));
        assertEq(vm.parseJsonAddress(json, ".oft_adapter"), address(m.adapter));
        assertEq(vm.parseJsonUint(json, ".exit_dst_eid"), ARB_EID);
        assertEq(vm.parseJsonBytes(json, ".exit_extra_options"), EXTRA_OPTIONS);

        // The core keys are unchanged by the two extra CREATEs.
        assertEq(vm.parseJsonUint(json, ".deployer_nonce"), 0);
        assertEq(vm.parseJsonAddress(json, ".vault"), address(d.vault));
        assertEq(vm.parseJsonAddress(json, ".relay_batch"), address(d.batch));
    }

    function test_VerifyDeployment_CatchesForwarderMismatch() public {
        LzMocks memory m = _lzMocks();
        DeployV2.Params memory p = _forwarderParams(address(m.adapter));
        DeployV2.Deployment memory d = script.deploy(p);
        script.verifyDeployment(p, d);

        // Addresses: F_d is the vault's predicted nonce-4 factory, F_x the nonce-5 CREATE.
        B3OftExitFactory exitFactory = d.exitFactory;
        d.exitFactory = new B3OftExitFactory(address(usdt), address(m.adapter), ARB_EID, EXTRA_OPTIONS);
        _expectPostCheck(p, d, "exit forwarder factory address");
        d.exitFactory = exitFactory;

        B3DepositForwarderFactory depositFactory = d.depositFactory;
        d.depositFactory = B3DepositForwarderFactory(FD_FACTORY);
        _expectPostCheck(p, d, "deposit forwarder factory address");
        d.depositFactory = depositFactory;

        d.forwarderFactory = FD_FACTORY;
        _expectPostCheck(p, d, "deposit forwarder factory address");
        d.forwarderFactory = address(depositFactory);
        d.forwarderFactoryPredicted = false;
        _expectPostCheck(p, d, "deposit forwarder factory address");
        d.forwarderFactoryPredicted = true;

        // Recorded hashes.
        d.forwarderInitCodeHash = keccak256("another forwarder");
        _expectPostCheck(p, d, "forwarder init code hash");
        d.forwarderInitCodeHash = keccak256(type(B3Forwarder).creationCode);
        bytes32 codeHash = d.depositFactoryCodeHash;
        d.depositFactoryCodeHash = keccak256("another F_d runtime");
        _expectPostCheck(p, d, "deposit forwarder factory code");
        d.depositFactoryCodeHash = codeHash;
        codeHash = d.exitFactoryCodeHash;
        d.exitFactoryCodeHash = keccak256("another F_x runtime");
        _expectPostCheck(p, d, "exit forwarder factory code");
        d.exitFactoryCodeHash = codeHash;

        // F_x immutables against the inputs.
        address adapter = p.oftAdapter;
        p.oftAdapter = address(new MockOFTAdapter(address(usdt), address(m.endpointEth)));
        _expectPostCheck(p, d, "F_x ADAPTER");
        p.oftAdapter = adapter;
        p.exitDstEid = ETH_EID;
        _expectPostCheck(p, d, "F_x DST_EID");
        p.exitDstEid = ARB_EID;
        p.exitExtraOptions = hex"0003";
        _expectPostCheck(p, d, "F_x extraOptions");
        p.exitExtraOptions = EXTRA_OPTIONS;

        // Immutables are read from the code at the factory addresses: an F_d
        // of another vault etched over the nonce-4 factory is caught, as is
        // an F_x with another destination over the nonce-5 one.
        bytes memory runtime = address(depositFactory).code;
        vm.etch(address(depositFactory), address(_otherDepositFactory(d)).code);
        d.depositFactoryCodeHash = address(depositFactory).codehash;
        _expectPostCheck(p, d, "F_d VAULT");
        vm.etch(address(depositFactory), runtime);
        d.depositFactoryCodeHash = address(depositFactory).codehash;

        runtime = address(exitFactory).code;
        vm.etch(
            address(exitFactory),
            address(new B3OftExitFactory(address(usdt), address(m.adapter), ETH_EID, EXTRA_OPTIONS)).code
        );
        d.exitFactoryCodeHash = address(exitFactory).codehash;
        _expectPostCheck(p, d, "F_x DST_EID");
        vm.etch(address(exitFactory), runtime);
        d.exitFactoryCodeHash = address(exitFactory).codehash;

        // A deployment naming factories fails the post-check without forwarders.
        p.deployForwarders = false;
        _expectPostCheck(p, d, "no forwarder factories");
        p.deployForwarders = true;

        script.verifyDeployment(p, d);
    }

    /// A default deployment whose record names a factory fails as well.
    function test_VerifyDeployment_DefaultRefusesForwarderFields() public {
        DeployV2.Params memory p = script.readParams();
        DeployV2.Deployment memory d = script.deploy(p);
        d.exitFactory = B3OftExitFactory(address(0xF0F0));
        _expectPostCheck(p, d, "no forwarder factories");
        d.exitFactory = B3OftExitFactory(address(0));
        d.forwarderInitCodeHash = keccak256(type(B3Forwarder).creationCode);
        _expectPostCheck(p, d, "no forwarder factories");
        d.forwarderInitCodeHash = bytes32(0);
        script.verifyDeployment(p, d);
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

    function _word(address a) private pure returns (bytes32) {
        return bytes32(uint256(uint160(a)));
    }

    function _lzMocks() private returns (LzMocks memory m) {
        m.endpointEth = new MockLzEndpoint(ETH_EID);
        m.endpointArb = new MockLzEndpoint(ARB_EID);
        m.adapter = new MockOFTAdapter(address(usdt), address(m.endpointEth));
        m.usdt0 = new MockUSDT0(address(m.endpointArb));
        m.adapter.setPeer(ARB_EID, _word(address(m.usdt0)));
        m.usdt0.setPeer(ETH_EID, _word(address(m.adapter)));
    }

    /// The environment's parameters plus forwarders over `adapter`, with the
    /// default DST_EID (30110) and EXTRA_OPTIONS.
    function _forwarderParams(address adapter) private view returns (DeployV2.Params memory p) {
        p = script.readParams();
        p.deployForwarders = true;
        p.oftAdapter = adapter;
        p.exitExtraOptions = EXTRA_OPTIONS;
    }

    /// Route (b) on the mocks: `user` sends `amount` USDT0 from Arbitrum to
    /// `to` on Ethereum (minAmountLD = amountLD, no compose); the harness
    /// relayer delivers it and the adapter, seeded with the backing, pays
    /// `to` in USDT by plain transfer.
    function _bridgeFromArbitrum(LzMocks memory m, address user, address to, uint256 amount) private {
        usdt.issue(amount);
        usdt.transfer(address(m.adapter), amount);
        m.usdt0.mint(user, amount);
        uint256 nativeFee = m.endpointArb.quote();
        vm.deal(user, nativeFee);
        IOFTV2.SendParam memory sp = IOFTV2.SendParam({
            dstEid: ETH_EID,
            to: _word(to),
            amountLD: amount,
            minAmountLD: amount,
            extraOptions: "",
            composeMsg: "",
            oftCmd: ""
        });
        vm.recordLogs();
        vm.prank(user, user);
        m.usdt0.send{value: nativeFee}(sp, IOFTV2.MessagingFee(nativeFee, 0), user);
        LzPacketsV2.only(vm.getRecordedLogs(), address(m.endpointArb)).deliver(m.endpointEth);
    }

    /// An F_d factory of a second vault on the same verifier and token,
    /// deployed by this contract at its predicted address.
    function _otherDepositFactory(DeployV2.Deployment memory d) private returns (B3DepositForwarderFactory) {
        address predicted = CHEATS.computeCreateAddress(address(this), uint256(CHEATS.getNonce(address(this))) + 1);
        B3VaultV2 other = new B3VaultV2(
            B3VaultV2.Config({
                verifier: d.verifier,
                verifierCodeHash: d.verifierCodeHash,
                b3ChainDomain: DOMAIN,
                originToken: address(usdt),
                tokenCodeHash: address(usdt).codehash,
                forwarderFactory: predicted,
                maxDepositRaw: CAP,
                predecessor: address(0),
                predecessorAssetId: bytes32(0)
            })
        );
        B3DepositForwarderFactory fd = new B3DepositForwarderFactory(address(other), address(other).codehash);
        require(address(fd) == predicted, "F_d factory at the predicted address");
        return fd;
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
