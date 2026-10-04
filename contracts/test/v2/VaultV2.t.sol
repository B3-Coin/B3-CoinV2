// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {V2TestBase, VmV2} from "./V2TestBase.sol";
import {MerkleTreeV2} from "./utils/MerkleTreeV2.sol";
import {MockVerifierV2} from "./mocks/MockVerifierV2.sol";
import {
    MockForwarderFactoryV2,
    ForwarderAccountV2,
    SafeLikeCallerV2,
    FalseReturnTokenV2,
    ReentrantTokenV2,
    NoFlagsTokenV2,
    OddFlagsTokenV2,
    IdentityStubV2
} from "./mocks/VaultMocksV2.sol";
import {B3TypesV2} from "../../src/v2/B3TypesV2.sol";
import {B3VaultV2} from "../../src/v2/B3VaultV2.sol";
import {IB3FinalityVerifierV2} from "../../src/v2/IB3FinalityVerifierV2.sol";
import {MockTetherToken} from "../../regtest/MockTetherToken.sol";
import {MockDepositVerifier} from "../../regtest/MockDepositVerifier.sol";
import {B3StakerBridge} from "../../src/B3StakerBridge.sol";
import {B3FinalityVerifier} from "../../src/B3FinalityVerifier.sol";

/// B3VaultV2 (contracts spec §6): constructor guards, the deposit gate and
/// caller rule, V1's Deposit event, leaf V2, release against the root
/// history, failed transfers that consume nothing, absorb, a B-23 migration
/// and the transient reentrancy lock. The verifier is MockVerifierV2 (the
/// vault reads only CHAIN_DOMAIN, BRIDGE_ACTIVATION_HEIGHT, depositsOpen and
/// acceptedRootHeight); test/v2/IntegrationV2.t.sol runs the real verifier.
contract VaultV2Test is V2TestBase {
    using MerkleTreeV2 for bytes32[];

    bytes32 private constant DOMAIN = keccak256("B3/TEST/BRIDGE/V2/VAULT-DOMAIN");
    bytes32 private constant MAINNET_DOMAIN = 0xa651262ae9031646047f290513d77ccae0fa9bd4e5afe7e07155a08d5dd1486a;
    uint64 private constant V = 311;
    /// 10,000 USDT in raw units (B-14, C-9).
    uint256 private constant CAP = 10_000_000_000;
    uint256 private constant UNIT = 1_000_000;

    address private constant USDT = 0xdAC17F958D2ee523a2206206994597C13D831ec7;
    address private constant ALICE = address(0xA11CE0);
    address private constant BOB = address(0xB0B0B0);
    address private constant CAROL = address(0xCA201C);
    address private constant BUNDLER = address(0xB4D1E5);
    bytes32 private constant RECIPIENT =
        bytes32((uint256(1) << 160) | uint256(uint160(0x5151515151515151515151515151515151515151)));

    /// keccak256("Deposit(uint64,address,uint256,bytes32)"), V1's topic 0 (spec §6.2).
    bytes32 private constant DEPOSIT_TOPIC = 0xdaf0af297d25c0e96a0b209d35692b4e07c503634eeca57fc5c35c006acf527f;

    MockVerifierV2 private verifier;
    MockTetherToken private usdt;
    MockForwarderFactoryV2 private factory;
    B3VaultV2 private vault;

    function setUp() public {
        verifier = new MockVerifierV2(DOMAIN, V);
        verifier.setDepositsOpen(true);
        usdt = new MockTetherToken();
        factory = new MockForwarderFactoryV2();
        vault = new B3VaultV2(_cfg(address(usdt), address(factory)));
    }

    // ============================================================ constructor

    function test_ConstructorPinsEveryImmutable() public view {
        assertEq(address(vault.VERIFIER()), address(verifier));
        assertEq(vault.VERIFIER_CODE_HASH(), address(verifier).codehash);
        assertEq(vault.B3_CHAIN_DOMAIN(), DOMAIN);
        assertEq(vault.ORIGIN_TOKEN(), address(usdt));
        assertEq(vault.TOKEN_CODE_HASH(), address(usdt).codehash);
        assertEq(vault.FORWARDER_FACTORY(), address(factory));
        assertEq(vault.MAX_DEPOSIT_RAW(), CAP);
        assertEq(uint256(vault.BRIDGE_ACTIVATION_HEIGHT()), V);
        assertEq(uint256(vault.ORIGIN_CHAIN_ID()), block.chainid);
        assertEq(vault.PREDECESSOR(), address(0));
        assertEq(vault.LEAF_TAG_V2(), keccak256("B3/BRIDGE/WITHDRAWAL/V2"));
        assertEq(uint256(vault.nextDepositId()), 0);
        assertEq(vault.locked(), 0);
        assertEq(vault.absorbedTotal(), 0);
    }

    function test_LaunchVaultAssetIdIsTheLibraryValue() public view {
        bytes32 expected = B3TypesV2.assetId(DOMAIN, uint64(block.chainid), address(vault), address(usdt));
        assertEq(vault.B3_ASSET_ID(), expected);
        // Independent 147-byte preimage: tag ‖ tag ‖ domain ‖ 0x02 ‖ LE64(chain) ‖ vault ‖ token ‖ 0x06 ‖ 0x06.
        bytes32 tag = sha256("B3/BRIDGE/ASSET/V2");
        bytes memory preimage = abi.encodePacked(
            tag, tag, DOMAIN, uint8(2), _le64(uint64(block.chainid)), address(vault), address(usdt), uint8(6), uint8(6)
        );
        assertEq(preimage.length, 147);
        assertEq(vault.B3_ASSET_ID(), sha256(preimage));
    }

    /// The real vault code at the vector address on chain 1 reproduces the
    /// committed AssetId V2 vector (constructor run in place, as forge-std's
    /// deployCodeTo does, so address(this) is the vector vault).
    function test_AssetIdVectorFromTheVaultItself() public {
        string memory j = readVectorsV2();
        address vectorVault = vm.parseJsonAddress(j, ".asset_id_v2[0].vault");
        assertEq(vm.parseJsonBytes32(j, ".asset_id_v2[0].chain_domain"), MAINNET_DOMAIN);
        assertEq(vm.parseJsonUint(j, ".asset_id_v2[0].origin_chain_id"), 1);
        assertEq(vm.parseJsonAddress(j, ".asset_id_v2[0].token"), USDT);

        vm.chainId(1);
        vm.etch(USDT, address(usdt).code);
        MockVerifierV2 mainnetVerifier = new MockVerifierV2(MAINNET_DOMAIN, V);
        B3VaultV2.Config memory cfg = _cfgFor(mainnetVerifier, MAINNET_DOMAIN, USDT, address(factory));
        B3VaultV2 placed = _deployAt(vectorVault, cfg);

        assertEq(placed.B3_ASSET_ID(), vm.parseJsonBytes32(j, ".asset_id_v2[0].asset_id"));
        assertEq(placed.B3_ASSET_ID(), 0xdb9e32397f3656fb2317c7829429f430cfefcf665c08a26774cb7b2fb3c76fcd);
    }

    function test_ConstructorRejectsBadVerifier() public {
        B3VaultV2.Config memory cfg = _cfg(address(usdt), address(factory));
        cfg.verifier = IB3FinalityVerifierV2(address(0));
        _expectBadConfig(cfg);

        cfg = _cfg(address(usdt), address(factory));
        cfg.verifier = IB3FinalityVerifierV2(address(0xC0DE1E55));
        vm.deal(address(cfg.verifier), 1);
        cfg.verifierCodeHash = keccak256("");
        assertEq(address(cfg.verifier).codehash, keccak256(""));
        _expectBadConfig(cfg);

        cfg = _cfg(address(usdt), address(factory));
        cfg.verifierCodeHash = bytes32(0);
        _expectBadConfig(cfg);

        cfg = _cfg(address(usdt), address(factory));
        cfg.verifierCodeHash = keccak256("another runtime");
        _expectBadConfig(cfg);

        // A verifier of the same code but another domain has another code hash
        // (immutables), and its domain differs anyway.
        MockVerifierV2 other = new MockVerifierV2(keccak256("other domain"), V);
        cfg = _cfgFor(other, DOMAIN, address(usdt), address(factory));
        _expectBadConfig(cfg);

        // Pinned code that does not answer CHAIN_DOMAIN() (the token).
        cfg = _cfg(address(usdt), address(factory));
        cfg.verifier = IB3FinalityVerifierV2(address(usdt));
        cfg.verifierCodeHash = address(usdt).codehash;
        _expectBadConfig(cfg);

        // A verifier whose V is zero.
        MockVerifierV2 zeroV = new MockVerifierV2(DOMAIN, 0);
        _expectBadConfig(_cfgFor(zeroV, DOMAIN, address(usdt), address(factory)));
    }

    function test_ConstructorRejectsBadDomain() public {
        B3VaultV2.Config memory cfg = _cfg(address(usdt), address(factory));
        cfg.b3ChainDomain = bytes32(0);
        _expectBadConfig(cfg);

        cfg = _cfg(address(usdt), address(factory));
        cfg.b3ChainDomain = keccak256("not the verifier's domain");
        _expectBadConfig(cfg);

        // A zero domain is refused even when the verifier agrees.
        MockVerifierV2 zeroDomain = new MockVerifierV2(bytes32(0), V);
        _expectBadConfig(_cfgFor(zeroDomain, bytes32(0), address(usdt), address(factory)));
    }

    function test_ConstructorRejectsBadTokenFactoryAndCap() public {
        B3VaultV2.Config memory cfg = _cfg(address(usdt), address(factory));
        cfg.originToken = address(0);
        cfg.tokenCodeHash = address(0).codehash;
        _expectBadConfig(cfg);

        // A codeless account that exists (it holds ETH) hashes to keccak256(""):
        // pinning that hash does not stand in for token code.
        cfg = _cfg(address(usdt), address(factory));
        cfg.originToken = address(0x70CE5);
        vm.deal(cfg.originToken, 1);
        cfg.tokenCodeHash = keccak256("");
        assertEq(cfg.originToken.codehash, keccak256(""));
        _expectBadConfig(cfg);

        cfg = _cfg(address(usdt), address(factory));
        cfg.tokenCodeHash = bytes32(0);
        _expectBadConfig(cfg);

        cfg = _cfg(address(usdt), address(factory));
        cfg.tokenCodeHash = address(new NoFlagsTokenV2()).codehash;
        _expectBadConfig(cfg);

        cfg = _cfg(address(usdt), address(0));
        _expectBadConfig(cfg);

        cfg = _cfg(address(usdt), address(factory));
        cfg.maxDepositRaw = 0;
        _expectBadConfig(cfg);

        // The launch vault takes no predecessor AssetId.
        cfg = _cfg(address(usdt), address(factory));
        cfg.predecessorAssetId = vault.B3_ASSET_ID();
        _expectBadConfig(cfg);
    }

    /// The F_d factory is deployed after the vault (its constructor checks
    /// VAULT.FORWARDER_FACTORY()), so the vault pins a predicted, codeless
    /// address. Until code exists there, only codeless EOAs may deposit.
    function test_CodelessPredictedFactoryAcceptsOnlyEoas() public {
        address predicted = address(0xFAC70A1);
        B3VaultV2 v = new B3VaultV2(_cfg(address(usdt), predicted));
        assertEq(v.FORWARDER_FACTORY(), predicted);
        _fund(ALICE, address(v), 2 * UNIT);
        vm.prank(ALICE, ALICE);
        v.deposit(UNIT, RECIPIENT);
        assertEq(v.locked(), UNIT);

        ForwarderAccountV2 fwd = new ForwarderAccountV2();
        usdt.issue(UNIT);
        usdt.transfer(address(fwd), UNIT);
        vm.expectRevert(B3VaultV2.BadCaller.selector);
        vm.prank(ALICE, ALICE);
        fwd.depositVia(address(v), address(usdt), UNIT, RECIPIENT);
    }

    function test_SuccessorConstructorChecksItsPredecessor() public {
        bytes32 id = vault.B3_ASSET_ID();

        // Accepted: same chain, token, domain and id.
        B3VaultV2 successor = new B3VaultV2(_successorCfg(address(vault), id));
        assertEq(successor.B3_ASSET_ID(), id);
        assertEq(successor.PREDECESSOR(), address(vault));
        // A successor's successor keeps the id.
        B3VaultV2 third = new B3VaultV2(_successorCfg(address(successor), id));
        assertEq(third.B3_ASSET_ID(), id);

        // Wrong or zero id.
        _expectBadConfig(_successorCfg(address(vault), keccak256("wrong id")));
        _expectBadConfig(_successorCfg(address(vault), bytes32(0)));

        // Codeless predecessor; a predecessor without the identity views.
        _expectBadConfig(_successorCfg(address(0x0DDBA11), id));
        _expectBadConfig(_successorCfg(address(usdt), id));

        // Wrong token: the successor names another USDT deployment.
        MockTetherToken otherToken = new MockTetherToken();
        B3VaultV2.Config memory cfg = _successorCfg(address(vault), id);
        cfg.originToken = address(otherToken);
        cfg.tokenCodeHash = address(otherToken).codehash;
        _expectBadConfig(cfg);

        // Wrong domain: a verifier and successor of another B3 chain domain.
        bytes32 otherDomain = keccak256("other B3 chain");
        MockVerifierV2 otherVerifier = new MockVerifierV2(otherDomain, V);
        otherVerifier.setDepositsOpen(true);
        cfg = _cfgFor(otherVerifier, otherDomain, address(usdt), address(factory));
        cfg.predecessor = address(vault);
        cfg.predecessorAssetId = id;
        _expectBadConfig(cfg);

        // Wrong chain: a predecessor deployed under another chain id.
        vm.chainId(5);
        B3VaultV2 foreign = new B3VaultV2(_cfg(address(usdt), address(factory)));
        bytes32 foreignId = foreign.B3_ASSET_ID();
        vm.chainId(31_337);
        _expectBadConfig(_successorCfg(address(foreign), foreignId));
        // The stub answers every view but the chain id.
        IdentityStubV2 stub = new IdentityStubV2(id, address(usdt), DOMAIN, 1);
        _expectBadConfig(_successorCfg(address(stub), id));
    }

    // =================================================================== gate

    function test_GateOpenWhenVerifierAndTokenAllow() public {
        assertTrue(vault.depositGateOpen());
        _fund(ALICE, address(vault), UNIT);
        vm.prank(ALICE, ALICE);
        vault.deposit(UNIT, RECIPIENT);
        assertEq(vault.locked(), UNIT);
        assertEq(usdt.balanceOf(address(vault)), UNIT);
    }

    function test_GateClosedByVerifier() public {
        _fund(ALICE, address(vault), UNIT);
        verifier.setDepositsOpen(false);
        assertFalse(vault.depositGateOpen());
        _expectDepositRevert(ALICE, UNIT, B3VaultV2.DepositClosed.selector);
        verifier.setDepositsOpen(true);
        assertTrue(vault.depositGateOpen());
    }

    function test_GateClosedWhileTokenPaused() public {
        _fund(ALICE, address(vault), UNIT);
        usdt.pause();
        assertFalse(vault.depositGateOpen());
        _expectDepositRevert(ALICE, UNIT, B3VaultV2.DepositClosed.selector);
        usdt.unpause();
        vm.prank(ALICE, ALICE);
        vault.deposit(UNIT, RECIPIENT);
    }

    function test_GateClosedOnceTokenDeprecated() public {
        _fund(ALICE, address(vault), UNIT);
        usdt.deprecate(address(0xDE9));
        assertFalse(vault.depositGateOpen());
        _expectDepositRevert(ALICE, UNIT, B3VaultV2.DepositClosed.selector);
    }

    function test_GateClosedWhileVaultBlacklisted() public {
        _fund(ALICE, address(vault), UNIT);
        usdt.addBlackList(address(vault));
        assertFalse(vault.depositGateOpen());
        _expectDepositRevert(ALICE, UNIT, B3VaultV2.DepositClosed.selector);
        usdt.removeBlackList(address(vault));
        assertTrue(vault.depositGateOpen());
        // A blacklisted depositor passes the gate; USDT then refuses transferFrom.
        usdt.addBlackList(ALICE);
        assertTrue(vault.depositGateOpen());
        _expectDepositRevert(ALICE, UNIT, B3VaultV2.TransferFailed.selector);
    }

    function test_GateClosedForTokenWithoutStatusViews() public {
        NoFlagsTokenV2 plain = new NoFlagsTokenV2();
        B3VaultV2 v = new B3VaultV2(_cfg(address(plain), address(factory)));
        plain.mint(ALICE, UNIT);
        vm.prank(ALICE);
        plain.approve(address(v), UNIT);
        assertFalse(v.depositGateOpen());
        vm.expectRevert(B3VaultV2.DepositClosed.selector);
        vm.prank(ALICE, ALICE);
        v.deposit(UNIT, RECIPIENT);
    }

    /// Each status view must answer exactly 32 bytes, all zero.
    function test_GateNeedsExactlyOneZeroWord() public {
        OddFlagsTokenV2 odd = new OddFlagsTokenV2();
        B3VaultV2 v = new B3VaultV2(_cfg(address(odd), address(factory)));
        assertTrue(v.depositGateOpen());

        odd.setPausedAnswer(abi.encode(uint256(0), uint256(0)));
        assertFalse(v.depositGateOpen(), "64 zero bytes");
        odd.setPausedAnswer("");
        assertFalse(v.depositGateOpen(), "empty answer");
        odd.setPausedAnswer(new bytes(31));
        assertFalse(v.depositGateOpen(), "31 bytes");
        odd.setPausedAnswer(abi.encode(uint256(1)));
        assertFalse(v.depositGateOpen(), "true");
        odd.setPausedAnswer(abi.encode(uint256(2)));
        assertFalse(v.depositGateOpen(), "non-boolean word");
        odd.setPausedAnswer(abi.encode(uint256(0)));
        assertTrue(v.depositGateOpen(), "false");
    }

    // ============================================================ caller rule

    function test_CallerRuleAcceptsCodelessTopLevelEoa() public {
        _fund(ALICE, address(vault), UNIT);
        vm.prank(ALICE, ALICE);
        vault.deposit(UNIT, RECIPIENT);
        assertEq(uint256(vault.nextDepositId()), 1);
    }

    function test_CallerRuleRefusesSafeLikeWallet() public {
        SafeLikeCallerV2 safe = new SafeLikeCallerV2();
        usdt.issue(UNIT);
        usdt.transfer(address(safe), UNIT);
        vm.prank(ALICE, ALICE);
        safe.exec(address(usdt), abi.encodeCall(MockTetherToken.approve, (address(vault), UNIT)));

        bytes memory depositCall = abi.encodeCall(B3VaultV2.deposit, (UNIT, RECIPIENT));
        vm.expectRevert(B3VaultV2.BadCaller.selector);
        vm.prank(ALICE, ALICE);
        safe.exec(address(vault), depositCall);
        // A bundler submitting the wallet's call fares no better.
        vm.expectRevert(B3VaultV2.BadCaller.selector);
        vm.prank(BUNDLER, BUNDLER);
        safe.exec(address(vault), depositCall);
    }

    function test_CallerRuleRefusesBundlerLikeOrigin() public {
        _fund(ALICE, address(vault), UNIT);
        // msg.sender is codeless but not the transaction origin.
        _expectDepositRevertAs(ALICE, BUNDLER, UNIT, B3VaultV2.BadCaller.selector);
        // The test contract itself (code, not the origin).
        vm.expectRevert(B3VaultV2.BadCaller.selector);
        vault.deposit(UNIT, RECIPIENT);
    }

    function test_CallerRuleRefusesEip7702Account() public {
        _fund(ALICE, address(vault), UNIT);
        SafeLikeCallerV2 implementation = new SafeLikeCallerV2();
        vm.etch(ALICE, abi.encodePacked(hex"ef0100", address(implementation)));
        assertEq(ALICE.code.length, 23);
        _expectDepositRevert(ALICE, UNIT, B3VaultV2.BadCaller.selector);
    }

    function test_CallerRuleAcceptsForwarderAccountOnly() public {
        ForwarderAccountV2 fwd = new ForwarderAccountV2();
        usdt.issue(10 * UNIT);
        usdt.transfer(address(fwd), 10 * UNIT);

        // Not an account of the factory.
        vm.expectRevert(B3VaultV2.BadCaller.selector);
        vm.prank(ALICE, ALICE);
        fwd.depositVia(address(vault), address(usdt), UNIT, RECIPIENT);

        // Registered: accepted, and the event is the ordinary Deposit.
        factory.setAccount(address(fwd), 1);
        vm.expectEmit(true, true, true, true, address(vault));
        emit B3StakerBridge.Deposit(0, address(usdt), UNIT, RECIPIENT);
        vm.prank(ALICE, ALICE);
        fwd.depositVia(address(vault), address(usdt), UNIT, RECIPIENT);
        assertEq(vault.locked(), UNIT);

        // Malformed factory answers refuse the caller.
        factory.setAccount(address(fwd), 2);
        vm.expectRevert(B3VaultV2.BadCaller.selector);
        vm.prank(ALICE, ALICE);
        fwd.depositVia(address(vault), address(usdt), UNIT, RECIPIENT);

        factory.setAccount(address(fwd), 1);
        factory.setMode(MockForwarderFactoryV2.Answer.LONG);
        vm.expectRevert(B3VaultV2.BadCaller.selector);
        vm.prank(ALICE, ALICE);
        fwd.depositVia(address(vault), address(usdt), UNIT, RECIPIENT);

        factory.setMode(MockForwarderFactoryV2.Answer.EMPTY);
        vm.expectRevert(B3VaultV2.BadCaller.selector);
        vm.prank(ALICE, ALICE);
        fwd.depositVia(address(vault), address(usdt), UNIT, RECIPIENT);

        factory.setMode(MockForwarderFactoryV2.Answer.REVERT);
        vm.expectRevert(B3VaultV2.BadCaller.selector);
        vm.prank(ALICE, ALICE);
        fwd.depositVia(address(vault), address(usdt), UNIT, RECIPIENT);

        factory.setMode(MockForwarderFactoryV2.Answer.WORD);
        vm.prank(ALICE, ALICE);
        fwd.depositVia(address(vault), address(usdt), UNIT, RECIPIENT);
        assertEq(vault.locked(), 2 * UNIT);
    }

    // ========================================================= deposit inputs

    function test_DepositRejectsZeroAmountAndBadRecipient() public {
        _fund(ALICE, address(vault), UNIT);
        _expectDepositRevertWith(ALICE, 0, RECIPIENT, B3VaultV2.ZeroAmount.selector);

        bytes32 hash20 = bytes32(uint256(uint160(0x5151515151515151515151515151515151515151)));
        _expectDepositRevertWith(ALICE, UNIT, bytes32(0), B3VaultV2.BadB3Recipient.selector);
        _expectDepositRevertWith(ALICE, UNIT, hash20, B3VaultV2.BadB3Recipient.selector); // version 0
        _expectDepositRevertWith(
            ALICE, UNIT, bytes32((uint256(2) << 160) | uint256(hash20)), B3VaultV2.BadB3Recipient.selector
        );
        _expectDepositRevertWith(ALICE, UNIT, bytes32(uint256(1) << 160), B3VaultV2.BadB3Recipient.selector);
        _expectDepositRevertWith(
            ALICE, UNIT, bytes32((uint256(1) << 255) | uint256(RECIPIENT)), B3VaultV2.BadB3Recipient.selector
        );
        _expectDepositRevertWith(
            ALICE, UNIT, bytes32((uint256(1) << 168) | uint256(RECIPIENT)), B3VaultV2.BadB3Recipient.selector
        );
        assertEq(vault.locked(), 0);
        assertEq(uint256(vault.nextDepositId()), 0);
    }

    function test_DepositCapAppliesToTheReceivedAmount() public {
        _fund(ALICE, address(vault), 2 * CAP + 1);
        _expectDepositRevert(ALICE, CAP + 1, B3VaultV2.DepositCapExceeded.selector);
        vm.prank(ALICE, ALICE);
        vault.deposit(CAP, RECIPIENT);
        assertEq(vault.locked(), CAP);

        // With a fee the cap binds what arrives, not what was sent.
        FalseReturnTokenV2 feeToken = new FalseReturnTokenV2();
        B3VaultV2 v = new B3VaultV2(_cfg(address(feeToken), address(factory)));
        feeToken.setMode(FalseReturnTokenV2.Mode.FEE);
        feeToken.setFee(10);
        feeToken.mint(ALICE, 3 * CAP);
        vm.prank(ALICE);
        feeToken.approve(address(v), type(uint256).max);

        vm.expectEmit(true, true, true, true, address(v));
        emit B3StakerBridge.Deposit(0, address(feeToken), CAP, RECIPIENT);
        vm.prank(ALICE, ALICE);
        v.deposit(CAP + 10, RECIPIENT);
        assertEq(v.locked(), CAP);

        vm.expectRevert(B3VaultV2.DepositCapExceeded.selector);
        vm.prank(ALICE, ALICE);
        v.deposit(CAP + 11, RECIPIENT);
    }

    function test_DepositRejectsSilentOrFalseTransfers() public {
        FalseReturnTokenV2 token = new FalseReturnTokenV2();
        B3VaultV2 v = new B3VaultV2(_cfg(address(token), address(factory)));
        token.mint(ALICE, UNIT);
        vm.prank(ALICE);
        token.approve(address(v), type(uint256).max);

        token.setMode(FalseReturnTokenV2.Mode.NO_OP);
        vm.expectRevert(B3VaultV2.ZeroAmount.selector);
        vm.prank(ALICE, ALICE);
        v.deposit(UNIT, RECIPIENT);

        token.setMode(FalseReturnTokenV2.Mode.RETURN_FALSE);
        vm.expectRevert(B3VaultV2.TransferFailed.selector);
        vm.prank(ALICE, ALICE);
        v.deposit(UNIT, RECIPIENT);

        token.setMode(FalseReturnTokenV2.Mode.NORMAL);
        vm.prank(ALICE, ALICE);
        v.deposit(UNIT, RECIPIENT);
        assertEq(v.locked(), UNIT);
    }

    // ========================================================== Deposit event

    /// Topics and data are byte-identical to V1's event, and the vault emits
    /// nothing else: the receipt holds the USDT Transfer and the Deposit only.
    function test_DepositLogIsByteIdenticalToV1() public {
        assertEq(DEPOSIT_TOPIC, keccak256("Deposit(uint64,address,uint256,bytes32)"));
        assertEq(DEPOSIT_TOPIC, B3StakerBridge.Deposit.selector);
        assertEq(DEPOSIT_TOPIC, B3VaultV2.Deposit.selector);

        // A real V1 vault on the same token (deposit-only regtest verifier).
        MockDepositVerifier v1Verifier = new MockDepositVerifier(DOMAIN, V);
        B3StakerBridge v1 = new B3StakerBridge(
            B3FinalityVerifier(address(v1Verifier)), address(v1Verifier).codehash, DOMAIN, address(usdt), CAP
        );
        uint256 amount = 1_234_567;
        _fund(ALICE, address(vault), 3 * amount);
        vm.prank(ALICE);
        usdt.approve(address(v1), type(uint256).max);

        vm.recordLogs();
        vm.prank(ALICE, ALICE);
        vault.deposit(amount, RECIPIENT);
        VmV2.Log[] memory v2Logs = vm.getRecordedLogs();
        vm.prank(ALICE, ALICE);
        v1.deposit(amount, RECIPIENT);
        VmV2.Log[] memory v1Logs = vm.getRecordedLogs();

        assertEq(v2Logs.length, 2, "Transfer and Deposit only");
        assertEq(v2Logs[0].emitter, address(usdt));
        assertEq(v2Logs[1].emitter, address(vault));
        VmV2.Log memory d2 = v2Logs[1];
        VmV2.Log memory d1 = v1Logs[v1Logs.length - 1];
        assertEq(d1.emitter, address(v1));

        assertEq(d2.topics.length, 3);
        assertEq(d1.topics.length, 3);
        assertEq(d2.topics[0], DEPOSIT_TOPIC);
        assertEq(d2.topics[1], bytes32(uint256(0)));
        assertEq(d2.topics[2], bytes32(uint256(uint160(address(usdt)))));
        assertEq(d2.data, abi.encode(amount, RECIPIENT));
        assertEq(d2.data.length, 64);
        for (uint256 i = 0; i < 3; ++i) {
            assertEq(d2.topics[i], d1.topics[i]);
        }
        assertEq(d2.data, d1.data);

        // Ids count up from zero.
        vm.recordLogs();
        vm.prank(ALICE, ALICE);
        vault.deposit(amount, RECIPIENT);
        v2Logs = vm.getRecordedLogs();
        assertEq(v2Logs[1].topics[1], bytes32(uint256(1)));
        assertEq(uint256(vault.nextDepositId()), 2);
    }

    // ================================================================ leaf V2

    function test_LeafV2VectorMatchesTheLibrary() public view {
        string memory j = readVectorsV2();
        bytes32 vectorLeaf = vm.parseJsonBytes32(j, ".leaf_v2[0].leaf");
        assertEq(vectorLeaf, 0x1cb905899ca0c488e158585c0208497d5415d1645035fbca3ca98ee2efb5ad64);
        assertEq(
            B3TypesV2.withdrawalLeaf(
                bytes32(0x1111111111111111111111111111111111111111111111111111111111111111),
                7,
                1,
                0x2222222222222222222222222222222222222222,
                USDT,
                0x3333333333333333333333333333333333333333,
                1_000_000,
                900_000
            ),
            vectorLeaf
        );

        // The vault's own leaf is the 180-byte layout over its immutables.
        B3TypesV2.WithdrawalV2 memory w = _w(7, BOB, 1_000_000, 900_000);
        bytes memory preimage = abi.encodePacked(
            keccak256("B3/BRIDGE/WITHDRAWAL/V2"),
            vault.B3_ASSET_ID(),
            uint64(7),
            uint64(block.chainid),
            address(vault),
            address(usdt),
            BOB,
            uint256(1_000_000),
            uint64(900_000)
        );
        assertEq(preimage.length, 180);
        assertEq(vault.withdrawalLeaf(w), keccak256(preimage));
        assertEq(vault.withdrawalLeaf(w), _leafOf(vault, w));
    }

    /// The real vault code at the vector address, on chain 1, with the vector's
    /// stand-in AssetId (adopted through a predecessor stub) and token,
    /// computes the spec §6.4 leaf and releases ids 0 and 7 of the committed
    /// vector tree against the committed root.
    function test_VaultReleasesTheCommittedVectorTree() public {
        string memory j = readVectorsV2();
        string memory p = ".withdrawal_tree_v2";
        bytes32 assetId = vm.parseJsonBytes32(j, string.concat(p, ".asset_id"));
        address vectorVault = vm.parseJsonAddress(j, string.concat(p, ".vault"));
        address recipient = vm.parseJsonAddress(j, string.concat(p, ".recipient"));
        uint256 amount = vm.parseJsonUint(j, string.concat(p, ".amount"));
        uint64 b3Height = uint64(vm.parseJsonUint(j, string.concat(p, ".b3_height")));
        bytes32 root = vm.parseJsonBytes32(j, string.concat(p, ".root"));
        assertEq(vm.parseJsonAddress(j, string.concat(p, ".token")), USDT);
        assertEq(vm.parseJsonUint(j, string.concat(p, ".chain_id")), 1);

        vm.chainId(1);
        vm.etch(USDT, address(usdt).code);
        MockVerifierV2 mainnetVerifier = new MockVerifierV2(MAINNET_DOMAIN, V);
        mainnetVerifier.setDepositsOpen(true);
        IdentityStubV2 stub = new IdentityStubV2(assetId, USDT, MAINNET_DOMAIN, 1);
        B3VaultV2.Config memory cfg = _cfgFor(mainnetVerifier, MAINNET_DOMAIN, USDT, address(factory));
        cfg.predecessor = address(stub);
        cfg.predecessorAssetId = assetId;
        B3VaultV2 placed = _deployAt(vectorVault, cfg);
        assertEq(placed.B3_ASSET_ID(), assetId);

        B3TypesV2.WithdrawalV2 memory w7 = _w(7, recipient, amount, b3Height);
        assertEq(placed.withdrawalLeaf(w7), vm.parseJsonBytes32(j, ".leaf_v2[0].leaf"));

        // Reserve: USDT at the mainnet address has no owner here, so credit
        // ALICE's balance directly (MockTetherToken slot 2 = balances).
        vm.store(USDT, keccak256(abi.encode(ALICE, uint256(2))), bytes32(2 * amount));
        vm.prank(ALICE);
        MockTetherToken(USDT).approve(address(placed), type(uint256).max);
        vm.prank(ALICE, ALICE);
        placed.deposit(2 * amount, RECIPIENT);

        mainnetVerifier.acceptRoot(root, b3Height);
        for (uint256 k = 0; k < 2; ++k) {
            string memory q = jsonKey(string.concat(p, ".paths"), k, "");
            uint64 index = uint64(vm.parseJsonUint(j, string.concat(q, ".index")));
            bytes32[32] memory path = MerkleTreeV2.toFixed32(vm.parseJsonBytes32Array(j, string.concat(q, ".siblings")));
            B3TypesV2.WithdrawalV2 memory w = _w(index, recipient, amount, b3Height);
            assertEq(placed.withdrawalLeaf(w), vm.parseJsonBytes32(j, string.concat(q, ".leaf")));
            placed.release(w, path);
            assertTrue(placed.released(index));
        }
        assertEq(MockTetherToken(USDT).balanceOf(recipient), 2 * amount);
        assertEq(placed.locked(), 0);
    }

    // ================================================================ release

    function test_ReleaseAgainstOlderRootAfterNewerRootAccepted() public {
        _fund(ALICE, address(vault), 3_000 * UNIT);
        vm.startPrank(ALICE, ALICE);
        vault.deposit(1_000 * UNIT, RECIPIENT);
        vault.deposit(1_000 * UNIT, RECIPIENT);
        vault.deposit(1_000 * UNIT, RECIPIENT);
        vm.stopPrank();

        // Global ids: 0 and 2 belong to this vault, 1 to another vault.
        B3TypesV2.WithdrawalV2 memory w0 = _w(0, BOB, 100 * UNIT, V);
        B3TypesV2.WithdrawalV2 memory w2 = _w(2, CAROL, 200 * UNIT, V + 10);
        bytes32[] memory leaves = new bytes32[](3);
        leaves[0] = _leafOf(vault, w0);
        leaves[1] = keccak256("a leaf of another vault");
        leaves[2] = _leafOf(vault, w2);

        bytes32 olderRoot = leaves.root(1, 32);
        bytes32 newerRoot = leaves.root(3, 32);
        verifier.acceptRoot(olderRoot, V + 5);
        verifier.acceptRoot(newerRoot, V + 15);

        // Id 0 under the older root, after the newer one was accepted.
        vm.expectEmit(true, true, true, true, address(vault));
        emit B3StakerBridge.Withdrawn(0, address(usdt), BOB, 100 * UNIT, V);
        vault.release(w0, leaves.path32(1, 0));
        assertEq(usdt.balanceOf(BOB), 100 * UNIT);
        assertTrue(vault.released(0));

        // Id 2 is not under the older root; it is under the newer one.
        bytes32[32] memory pathInNewer = leaves.path32(3, 2);
        bytes32[32] memory pathInOlder = leaves.path32(1, 2);
        vm.expectRevert(B3VaultV2.UnknownRoot.selector);
        vault.release(w2, pathInOlder);
        vault.release(w2, pathInNewer);
        assertEq(usdt.balanceOf(CAROL), 200 * UNIT);
        assertEq(vault.locked(), 2_700 * UNIT);
        assertEq(usdt.balanceOf(address(vault)), 2_700 * UNIT);

        // The same leaf under the newer root is still one payment right.
        bytes32[32] memory w0InNewer = leaves.path32(3, 0);
        vm.expectRevert(B3VaultV2.AlreadyReleased.selector);
        vault.release(w0, w0InNewer);
        vm.expectRevert(B3VaultV2.AlreadyReleased.selector);
        vault.release(w2, pathInNewer);
        assertFalse(vault.released(1));
    }

    function test_ReleaseRejectsBadLeaves() public {
        _fund(ALICE, address(vault), 1_000 * UNIT);
        vm.prank(ALICE, ALICE);
        vault.deposit(1_000 * UNIT, RECIPIENT);

        B3TypesV2.WithdrawalV2 memory w = _w(0, BOB, 100 * UNIT, V + 20);
        B3TypesV2.WithdrawalV2 memory big = _w(1, BOB, 1_000 * UNIT + 1, V + 20);
        bytes32[] memory leaves = new bytes32[](2);
        leaves[0] = _leafOf(vault, w);
        leaves[1] = _leafOf(vault, big);
        bytes32 root = leaves.root(2, 32);
        bytes32[32] memory path0 = leaves.path32(2, 0);
        bytes32[32] memory path1 = leaves.path32(2, 1);

        // Unknown root.
        vm.expectRevert(B3VaultV2.UnknownRoot.selector);
        vault.release(w, path0);

        // A root recorded below the leaf's height (never written by the real
        // verifier for an append-only tree) does not pay it.
        verifier.overwriteRootHeight(root, V + 19);
        vm.expectRevert(B3VaultV2.RootBelowLeafHeight.selector);
        vault.release(w, path0);
        verifier.overwriteRootHeight(root, V + 20);

        // Tampered fields or path: another leaf, another root.
        B3TypesV2.WithdrawalV2 memory t = _w(0, CAROL, 100 * UNIT, V + 20);
        vm.expectRevert(B3VaultV2.UnknownRoot.selector);
        vault.release(t, path0);
        t = _w(0, BOB, 100 * UNIT + 1, V + 20);
        vm.expectRevert(B3VaultV2.UnknownRoot.selector);
        vault.release(t, path0);
        t = _w(0, BOB, 100 * UNIT, V + 19);
        vm.expectRevert(B3VaultV2.UnknownRoot.selector);
        vault.release(t, path0);
        bytes32[32] memory bent = leaves.path32(2, 0);
        bent[31] = bytes32(uint256(bent[31]) ^ 1);
        vm.expectRevert(B3VaultV2.UnknownRoot.selector);
        vault.release(w, bent);
        // The right path at another index.
        t = _w(1, BOB, 100 * UNIT, V + 20);
        vm.expectRevert(B3VaultV2.UnknownRoot.selector);
        vault.release(t, path0);

        // Field bounds, checked before any proof work.
        t = _w(uint64(B3TypesV2.MAX_INDEX), BOB, 100 * UNIT, V + 20);
        vm.expectRevert(B3VaultV2.WithdrawalIdOutOfRange.selector);
        vault.release(t, path0);
        t = _w(type(uint64).max, BOB, 100 * UNIT, V + 20);
        vm.expectRevert(B3VaultV2.WithdrawalIdOutOfRange.selector);
        vault.release(t, path0);
        t = _w(0, BOB, 100 * UNIT, V - 1);
        vm.expectRevert(B3VaultV2.WithdrawalBeforeActivation.selector);
        vault.release(t, path0);
        t = _w(0, address(0), 100 * UNIT, V + 20);
        vm.expectRevert(B3VaultV2.ZeroRecipient.selector);
        vault.release(t, path0);
        t = _w(0, BOB, 0, V + 20);
        vm.expectRevert(B3VaultV2.ZeroAmount.selector);
        vault.release(t, path0);

        // A certified amount above the reserve waits for reserve.
        vm.expectRevert(B3VaultV2.InsufficientReserve.selector);
        vault.release(big, path1);
        assertFalse(vault.released(1));

        // The good leaf pays exactly once.
        vault.release(w, path0);
        vm.expectRevert(B3VaultV2.AlreadyReleased.selector);
        vault.release(w, path0);
        assertEq(usdt.balanceOf(BOB), 100 * UNIT);
        assertEq(vault.locked(), 900 * UNIT);
    }

    function test_ReleaseAtTheLastIdAndAtV() public {
        _fund(ALICE, address(vault), UNIT);
        vm.prank(ALICE, ALICE);
        vault.deposit(UNIT, RECIPIENT);

        uint64 lastId = uint64(B3TypesV2.MAX_INDEX - 1);
        B3TypesV2.WithdrawalV2 memory w = _w(lastId, BOB, UNIT, V);
        bytes32 leaf = _leafOf(vault, w);
        // Sparse tree with the leaf at the last position: siblings are zero subtrees.
        bytes32[] memory z = MerkleTreeV2.zeros(32);
        bytes32[32] memory path;
        bytes32 node = leaf;
        for (uint256 level = 0; level < 32; ++level) {
            path[level] = z[level];
            node = keccak256(abi.encodePacked(z[level], node));
        }
        verifier.acceptRoot(node, V);
        vault.release(w, path);
        assertTrue(vault.released(lastId));
        assertEq(usdt.balanceOf(BOB), UNIT);
    }

    /// Spec §6.4: a failing transfer consumes nothing; the leaf stays payable.
    function test_PausedTetherRevertsReleaseWholeThenPaysOnce() public {
        (B3TypesV2.WithdrawalV2 memory w, bytes32[32] memory path) = _fundedLeaf(vault, 500 * UNIT);
        uint256 lockedBefore = vault.locked();

        usdt.pause();
        vm.expectRevert(B3VaultV2.TransferFailed.selector);
        vault.release(w, path);
        assertFalse(vault.released(w.withdrawalId));
        assertEq(vault.locked(), lockedBefore);
        assertEq(usdt.balanceOf(BOB), 0);

        usdt.unpause();
        vault.release(w, path);
        assertTrue(vault.released(w.withdrawalId));
        assertEq(usdt.balanceOf(BOB), 500 * UNIT);
        assertEq(vault.locked(), lockedBefore - 500 * UNIT);
        vm.expectRevert(B3VaultV2.AlreadyReleased.selector);
        vault.release(w, path);
    }

    function test_BlacklistedVaultRevertsReleaseUntilReversed() public {
        (B3TypesV2.WithdrawalV2 memory w, bytes32[32] memory path) = _fundedLeaf(vault, 500 * UNIT);
        usdt.addBlackList(address(vault));
        vm.expectRevert(B3VaultV2.TransferFailed.selector);
        vault.release(w, path);
        assertFalse(vault.released(w.withdrawalId));
        usdt.removeBlackList(address(vault));
        vault.release(w, path);
        assertEq(usdt.balanceOf(BOB), 500 * UNIT);
    }

    /// USDT checks only the sender, so a release to a blacklisted recipient
    /// succeeds and the funds arrive frozen (spec §6.5); wallets pre-check.
    function test_ReleaseToBlacklistedRecipientArrives() public {
        (B3TypesV2.WithdrawalV2 memory w, bytes32[32] memory path) = _fundedLeaf(vault, 500 * UNIT);
        usdt.addBlackList(BOB);
        vault.release(w, path);
        assertEq(usdt.balanceOf(BOB), 500 * UNIT);
    }

    function test_FalseReturningTokenRevertsReleaseWholeThenPaysOnce() public {
        FalseReturnTokenV2 token = new FalseReturnTokenV2();
        B3VaultV2 v = new B3VaultV2(_cfg(address(token), address(factory)));
        token.mint(ALICE, 1_000 * UNIT);
        vm.prank(ALICE);
        token.approve(address(v), type(uint256).max);
        vm.prank(ALICE, ALICE);
        v.deposit(1_000 * UNIT, RECIPIENT);

        B3TypesV2.WithdrawalV2 memory w = _w(0, BOB, 400 * UNIT, V);
        bytes32[] memory leaves = new bytes32[](1);
        leaves[0] = _leafOf(v, w);
        verifier.acceptRoot(leaves.root(1, 32), V);
        bytes32[32] memory path = leaves.path32(1, 0);

        token.setMode(FalseReturnTokenV2.Mode.RETURN_FALSE);
        vm.expectRevert(B3VaultV2.TransferFailed.selector);
        v.release(w, path);
        assertFalse(v.released(0));
        assertEq(v.locked(), 1_000 * UNIT);

        token.setMode(FalseReturnTokenV2.Mode.NORMAL);
        v.release(w, path);
        assertEq(token.balanceOf(BOB), 400 * UNIT);
        vm.expectRevert(B3VaultV2.AlreadyReleased.selector);
        v.release(w, path);
    }

    /// With a token fee, release lowers locked by the gross amount and the
    /// recipient receives the net (spec §6.5).
    function test_FeeTokenReleaseLowersLockedByGross() public {
        FalseReturnTokenV2 token = new FalseReturnTokenV2();
        B3VaultV2 v = new B3VaultV2(_cfg(address(token), address(factory)));
        token.mint(ALICE, 1_000 * UNIT);
        vm.prank(ALICE);
        token.approve(address(v), type(uint256).max);
        vm.prank(ALICE, ALICE);
        v.deposit(1_000 * UNIT, RECIPIENT);

        token.setMode(FalseReturnTokenV2.Mode.FEE);
        token.setFee(7);
        B3TypesV2.WithdrawalV2 memory w = _w(0, BOB, 400 * UNIT, V);
        bytes32[] memory leaves = new bytes32[](1);
        leaves[0] = _leafOf(v, w);
        verifier.acceptRoot(leaves.root(1, 32), V);
        v.release(w, leaves.path32(1, 0));
        assertEq(token.balanceOf(BOB), 400 * UNIT - 7);
        assertEq(v.locked(), 600 * UNIT);
        assertEq(token.balanceOf(address(v)), 600 * UNIT);
    }

    /// Release ignores verifier status and the deposit gate: a halted or
    /// frozen verifier keeps paying roots it accepted before it stopped (C-1).
    function test_ReleaseIgnoresVerifierStatusAndTokenFlags() public {
        (B3TypesV2.WithdrawalV2 memory w, bytes32[32] memory path) = _fundedLeaf(vault, 300 * UNIT);
        verifier.setStatus(B3TypesV2.STATUS_HALTED_CONFLICT);
        verifier.setDepositsOpen(false);
        usdt.deprecate(address(0xDE9));
        assertFalse(vault.depositGateOpen());

        vault.release(w, path);
        assertEq(usdt.balanceOf(BOB), 300 * UNIT);

        verifier.setStatus(B3TypesV2.STATUS_CANCELLED);
        B3TypesV2.WithdrawalV2 memory w2 = _w(1, CAROL, 100 * UNIT, V + 1);
        bytes32[] memory leaves = new bytes32[](2);
        leaves[0] = _leafOf(vault, w);
        leaves[1] = _leafOf(vault, w2);
        verifier.acceptRoot(leaves.root(2, 32), V + 1);
        vault.release(w2, leaves.path32(2, 1));
        assertEq(usdt.balanceOf(CAROL), 100 * UNIT);
    }

    // ================================================================= absorb

    function test_AbsorbCountsExactlyTheIncrease() public {
        _fund(ALICE, address(vault), 1_000 * UNIT);
        vm.prank(ALICE, ALICE);
        vault.deposit(1_000 * UNIT, RECIPIENT);

        // No surplus: a no-op without an event.
        vm.recordLogs();
        assertEq(vault.absorb(), 0);
        assertEq(vm.getRecordedLogs().length, 0);
        assertEq(vault.locked(), 1_000 * UNIT);
        assertEq(vault.absorbedTotal(), 0);

        // A stray transfer (no Deposit, no B3 claim) is counted once.
        usdt.issue(37 * UNIT);
        usdt.transfer(address(vault), 37 * UNIT);
        vm.expectEmit(true, true, true, true, address(vault));
        emit B3VaultV2.Absorbed(37 * UNIT, 37 * UNIT);
        vm.prank(CAROL);
        assertEq(vault.absorb(), 37 * UNIT);
        assertEq(vault.locked(), 1_037 * UNIT);
        assertEq(vault.absorbedTotal(), 37 * UNIT);
        assertEq(vault.absorb(), 0);
        assertEq(vault.absorbedTotal(), 37 * UNIT);

        usdt.issue(5);
        usdt.transfer(address(vault), 5);
        assertEq(vault.absorb(), 5);
        assertEq(vault.absorbedTotal(), 37 * UNIT + 5);
        assertEq(vault.locked(), 1_037 * UNIT + 5);

        // A balance below locked (e.g. seized funds) never lowers locked.
        vm.store(address(usdt), keccak256(abi.encode(address(vault), uint256(2))), bytes32(uint256(10 * UNIT)));
        assertEq(vault.absorb(), 0);
        assertEq(vault.locked(), 1_037 * UNIT + 5);
        assertEq(vault.absorbedTotal(), 37 * UNIT + 5);
    }

    // ============================================================== migration

    /// B-23: B3 appends an ordinary leaf naming the old vault v, paying the
    /// successor v′ that carries v's AssetId. v releases it once; v′ counts the
    /// arrival only through absorb(); v′ then pays its own leaves, and later
    /// deposits into the retired v are swept by further ordinary leaves.
    function test_MigrationLeafPaysSuccessorOnceAndAbsorbCountsIt() public {
        _fund(ALICE, address(vault), 3_000 * UNIT);
        vm.startPrank(ALICE, ALICE);
        vault.deposit(1_000 * UNIT, RECIPIENT);
        vault.deposit(2_000 * UNIT, RECIPIENT);
        vm.stopPrank();

        B3VaultV2 successor = new B3VaultV2(_successorCfg(address(vault), vault.B3_ASSET_ID()));
        assertEq(successor.B3_ASSET_ID(), vault.B3_ASSET_ID());

        uint256 reserve = vault.locked();
        B3TypesV2.WithdrawalV2 memory m = _w(5, address(successor), reserve, V + 100);
        bytes32[] memory leaves = new bytes32[](8);
        for (uint256 i = 0; i < 5; ++i) {
            leaves[i] = keccak256(abi.encode("earlier leaf", i));
        }
        leaves[5] = _leafOf(vault, m);
        verifier.acceptRoot(leaves.root(6, 32), V + 100);
        bytes32[32] memory mPath = leaves.path32(6, 5);

        // The successor computes another leaf for the same id; it cannot pay it.
        assertNotEq(successor.withdrawalLeaf(m), vault.withdrawalLeaf(m));
        vm.expectRevert(B3VaultV2.UnknownRoot.selector);
        successor.release(m, mPath);

        vm.expectEmit(true, true, true, true, address(vault));
        emit B3StakerBridge.Withdrawn(5, address(usdt), address(successor), reserve, V + 100);
        vault.release(m, mPath);
        assertEq(vault.locked(), 0);
        assertEq(usdt.balanceOf(address(successor)), reserve);
        vm.expectRevert(B3VaultV2.AlreadyReleased.selector);
        vault.release(m, mPath);

        // The arrival is not reserve until absorbed.
        assertEq(successor.locked(), 0);
        assertEq(successor.absorbedTotal(), 0);
        vm.expectEmit(true, true, true, true, address(successor));
        emit B3VaultV2.Absorbed(reserve, reserve);
        assertEq(successor.absorb(), reserve);
        assertEq(successor.locked(), reserve);
        assertEq(successor.absorbedTotal(), reserve);
        assertEq(successor.absorb(), 0);

        // The successor pays its own leaves; v cannot pay them.
        B3TypesV2.WithdrawalV2 memory w6 = _w(6, BOB, 250 * UNIT, V + 200);
        leaves[6] = _leafOf(successor, w6);
        verifier.acceptRoot(leaves.root(7, 32), V + 200);
        bytes32[32] memory path6 = leaves.path32(7, 6);
        vm.expectRevert(B3VaultV2.UnknownRoot.selector);
        vault.release(w6, path6);
        successor.release(w6, path6);
        assertEq(usdt.balanceOf(BOB), 250 * UNIT);
        assertEq(successor.locked(), reserve - 250 * UNIT);

        // A later deposit into the retired v (its gate reads only its verifier
        // and USDT) is minted on B3 and swept to v′ by a further ordinary leaf.
        _fund(CAROL, address(vault), 40 * UNIT);
        vm.prank(CAROL, CAROL);
        vault.deposit(40 * UNIT, RECIPIENT);
        B3TypesV2.WithdrawalV2 memory sweep = _w(7, address(successor), 40 * UNIT, V + 300);
        leaves[7] = _leafOf(vault, sweep);
        verifier.acceptRoot(leaves.root(8, 32), V + 300);
        vault.release(sweep, leaves.path32(8, 7));
        assertEq(successor.absorb(), 40 * UNIT);
        assertEq(successor.absorbedTotal(), reserve + 40 * UNIT);
        assertEq(successor.locked(), reserve - 250 * UNIT + 40 * UNIT);
        assertEq(vault.locked(), 0);

        // Old certified leaves of v still pay from what v holds (none left here).
        assertEq(usdt.balanceOf(address(vault)), 0);
    }

    // ============================================================ reentrancy

    function test_ReentrancyThroughTokenCallbackReverts() public {
        ReentrantTokenV2 token = new ReentrantTokenV2();
        B3VaultV2 v = new B3VaultV2(_cfg(address(token), address(factory)));
        token.mint(ALICE, 1_000 * UNIT);
        vm.prank(ALICE);
        token.approve(address(v), type(uint256).max);
        bytes memory reentrancy = abi.encodeWithSelector(B3VaultV2.Reentrancy.selector);

        // deposit -> transferFrom -> deposit.
        token.arm(address(v), abi.encodeCall(B3VaultV2.deposit, (UNIT, RECIPIENT)));
        vm.prank(ALICE, ALICE);
        v.deposit(500 * UNIT, RECIPIENT);
        assertTrue(token.hookFired());
        assertFalse(token.lastHookOk());
        assertEq(token.lastHookReturn(), reentrancy);
        assertEq(v.locked(), 500 * UNIT);
        assertEq(uint256(v.nextDepositId()), 1);

        // deposit -> transferFrom -> absorb.
        token.arm(address(v), abi.encodeCall(B3VaultV2.absorb, ()));
        vm.prank(ALICE, ALICE);
        v.deposit(500 * UNIT, RECIPIENT);
        assertFalse(token.lastHookOk());
        assertEq(token.lastHookReturn(), reentrancy);
        assertEq(v.absorbedTotal(), 0);
        assertEq(v.locked(), 1_000 * UNIT);

        // release -> transfer -> release of the same leaf.
        B3TypesV2.WithdrawalV2 memory w = _w(0, BOB, 300 * UNIT, V);
        bytes32[] memory leaves = new bytes32[](1);
        leaves[0] = _leafOf(v, w);
        verifier.acceptRoot(leaves.root(1, 32), V);
        bytes32[32] memory path = leaves.path32(1, 0);
        token.arm(address(v), abi.encodeCall(B3VaultV2.release, (w, path)));
        v.release(w, path);
        assertFalse(token.lastHookOk());
        assertEq(token.lastHookReturn(), reentrancy);
        assertEq(token.balanceOf(BOB), 300 * UNIT);
        assertEq(v.locked(), 700 * UNIT);
        assertTrue(v.released(0));

        // The lock is released after each call: a later call in the same
        // transaction works.
        token.mint(address(v), 1);
        assertEq(v.absorb(), 1);
    }

    // ================================================================ helpers

    function _cfg(address token, address forwarderFactory) private view returns (B3VaultV2.Config memory) {
        return _cfgFor(verifier, DOMAIN, token, forwarderFactory);
    }

    function _cfgFor(MockVerifierV2 v, bytes32 domain, address token, address forwarderFactory)
        private
        view
        returns (B3VaultV2.Config memory)
    {
        return B3VaultV2.Config({
            verifier: IB3FinalityVerifierV2(address(v)),
            verifierCodeHash: address(v).codehash,
            b3ChainDomain: domain,
            originToken: token,
            tokenCodeHash: token.codehash,
            forwarderFactory: forwarderFactory,
            maxDepositRaw: CAP,
            predecessor: address(0),
            predecessorAssetId: bytes32(0)
        });
    }

    function _successorCfg(address predecessor, bytes32 assetId) private view returns (B3VaultV2.Config memory cfg) {
        cfg = _cfg(address(usdt), address(factory));
        cfg.predecessor = predecessor;
        cfg.predecessorAssetId = assetId;
    }

    function _expectBadConfig(B3VaultV2.Config memory cfg) private {
        vm.expectRevert(B3VaultV2.BadConfig.selector);
        new B3VaultV2(cfg);
    }

    /// Runs the constructor in place at `where` (forge-std deployCodeTo):
    /// address(this) inside the constructor is `where`.
    function _deployAt(address where, B3VaultV2.Config memory cfg) private returns (B3VaultV2) {
        vm.etch(where, abi.encodePacked(type(B3VaultV2).creationCode, abi.encode(cfg)));
        (bool ok, bytes memory runtime) = where.call("");
        assertTrue(ok, "in-place constructor");
        vm.etch(where, runtime);
        return B3VaultV2(where);
    }

    /// Gives `who` `amount` USDT and an unlimited allowance for `spender`.
    function _fund(address who, address spender, uint256 amount) private {
        usdt.issue(amount);
        usdt.transfer(who, amount);
        vm.prank(who);
        usdt.approve(spender, type(uint256).max);
    }

    function _expectDepositRevert(address eoa, uint256 amount, bytes4 selector) private {
        _expectDepositRevertWith(eoa, amount, RECIPIENT, selector);
    }

    function _expectDepositRevertWith(address eoa, uint256 amount, bytes32 recipient, bytes4 selector) private {
        vm.expectRevert(selector);
        vm.prank(eoa, eoa);
        vault.deposit(amount, recipient);
    }

    function _expectDepositRevertAs(address sender, address origin, uint256 amount, bytes4 selector) private {
        vm.expectRevert(selector);
        vm.prank(sender, origin);
        vault.deposit(amount, RECIPIENT);
    }

    /// Deposits 1,000 units and certifies one leaf of `amount` to BOB (id 0, height V).
    function _fundedLeaf(B3VaultV2 v, uint256 amount)
        private
        returns (B3TypesV2.WithdrawalV2 memory w, bytes32[32] memory path)
    {
        _fund(ALICE, address(v), 1_000 * UNIT);
        vm.prank(ALICE, ALICE);
        v.deposit(1_000 * UNIT, RECIPIENT);
        w = _w(0, BOB, amount, V);
        bytes32[] memory leaves = new bytes32[](1);
        leaves[0] = _leafOf(v, w);
        verifier.acceptRoot(leaves.root(1, 32), V);
        path = leaves.path32(1, 0);
    }

    function _w(uint64 id, address recipient, uint256 amount, uint64 b3Height)
        private
        pure
        returns (B3TypesV2.WithdrawalV2 memory)
    {
        return B3TypesV2.WithdrawalV2({withdrawalId: id, recipient: recipient, amount: amount, b3Height: b3Height});
    }

    /// Leaf V2 computed in the test from the vault's public immutables.
    function _leafOf(B3VaultV2 v, B3TypesV2.WithdrawalV2 memory w) private view returns (bytes32) {
        return keccak256(
            abi.encodePacked(
                keccak256("B3/BRIDGE/WITHDRAWAL/V2"),
                v.B3_ASSET_ID(),
                w.withdrawalId,
                uint64(block.chainid),
                address(v),
                v.ORIGIN_TOKEN(),
                w.recipient,
                w.amount,
                w.b3Height
            )
        );
    }

    /// Little-endian 8 bytes of x (the AssetId preimage's origin chain id).
    function _le64(uint64 x) private pure returns (bytes8) {
        bytes memory out = new bytes(8);
        for (uint256 i = 0; i < 8; ++i) {
            // forge-lint: disable-next-line(unsafe-typecast)
            out[i] = bytes1(uint8(x >> (8 * i)));
        }
        // forge-lint: disable-next-line(unsafe-typecast)
        return bytes8(out);
    }
}
