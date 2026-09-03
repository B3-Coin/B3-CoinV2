// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {TestBase} from "./TestBase.sol";
import {B3Types, IB3FinalityProver} from "../src/IB3FinalityProver.sol";
import {B3FinalityVerifier} from "../src/B3FinalityVerifier.sol";
import {B3StakerBridge} from "../src/B3StakerBridge.sol";
import {BlsCertificateProver} from "../src/BlsCertificateProver.sol";

contract MockFinalityProver is IB3FinalityProver {
    bool public result = true;

    function setResult(bool value) external {
        result = value;
    }

    function verify(bytes32, B3Types.FinalizedBlock calldata, bytes32, B3Types.SetHeader calldata, bytes calldata)
        external
        view
        returns (bool)
    {
        return result;
    }
}

/// USDT-shaped mock: approve/transfer/transferFrom return no value.
contract MockBridgeUSDT {
    mapping(address => uint256) public balanceOf;
    mapping(address => mapping(address => uint256)) public allowance;

    function mint(address account, uint256 amount) external {
        balanceOf[account] += amount;
    }

    function approve(address spender, uint256 amount) external {
        allowance[msg.sender][spender] = amount;
    }

    function transfer(address recipient, uint256 amount) external {
        balanceOf[msg.sender] -= amount;
        balanceOf[recipient] += amount;
    }

    function transferFrom(address sender, address recipient, uint256 amount) external {
        allowance[sender][msg.sender] -= amount;
        balanceOf[sender] -= amount;
        balanceOf[recipient] += amount;
    }
}

/// Fee-on-transfer token: the bridge receives 99% of the requested amount.
contract FeeBridgeToken {
    mapping(address => uint256) public balanceOf;
    mapping(address => mapping(address => uint256)) public allowance;

    function mint(address account, uint256 amount) external {
        balanceOf[account] += amount;
    }

    function approve(address spender, uint256 amount) external returns (bool) {
        allowance[msg.sender][spender] = amount;
        return true;
    }

    function transfer(address recipient, uint256 amount) external returns (bool) {
        balanceOf[msg.sender] -= amount;
        balanceOf[recipient] += amount;
        return true;
    }

    function transferFrom(address sender, address recipient, uint256 amount) external returns (bool) {
        allowance[sender][msg.sender] -= amount;
        balanceOf[sender] -= amount;
        balanceOf[recipient] += amount - amount / 100;
        return true;
    }
}

/// Token test double for custody failure paths. It can attempt one callback
/// during release, return false, or revert without changing balances.
contract HostileBridgeToken {
    enum TransferMode {
        SUCCEED,
        RETURN_FALSE,
        REVERT
    }

    mapping(address => uint256) public balanceOf;
    mapping(address => mapping(address => uint256)) public allowance;

    TransferMode public transferMode;
    address private _callbackTarget;
    bytes private _callbackData;
    bool private _callbackArmed;
    bool public callbackSucceeded;
    bytes4 public callbackError;

    function mint(address account, uint256 amount) external {
        balanceOf[account] += amount;
    }

    function approve(address spender, uint256 amount) external returns (bool) {
        allowance[msg.sender][spender] = amount;
        return true;
    }

    function setTransferMode(TransferMode mode) external {
        transferMode = mode;
    }

    function armCallback(address target, bytes calldata data) external {
        _callbackTarget = target;
        _callbackData = data;
        _callbackArmed = true;
        callbackSucceeded = false;
        callbackError = bytes4(0);
    }

    function transfer(address recipient, uint256 amount) external returns (bool) {
        if (transferMode == TransferMode.RETURN_FALSE) return false;
        if (transferMode == TransferMode.REVERT) revert("HOSTILE_TRANSFER");

        if (_callbackArmed) {
            _callbackArmed = false;
            bytes memory result;
            (callbackSucceeded, result) = _callbackTarget.call(_callbackData);
            if (!callbackSucceeded && result.length >= 4) {
                bytes4 selector;
                assembly {
                    selector := mload(add(result, 32))
                }
                callbackError = selector;
            }
        }

        balanceOf[msg.sender] -= amount;
        balanceOf[recipient] += amount;
        return true;
    }

    function transferFrom(address sender, address recipient, uint256 amount) external returns (bool) {
        allowance[sender][msg.sender] -= amount;
        balanceOf[sender] -= amount;
        balanceOf[recipient] += amount;
        return true;
    }
}

contract B3StakerBridgeTest is TestBase {
    bytes32 private constant DOMAIN = keccak256("fixture-chain-domain");
    // Constructor argument three is the B3 chain domain; the bridge derives
    // its AssetId from this value and its own deployed address.
    bytes32 private constant ASSET = DOMAIN;
    uint64 private constant M = 100;
    uint64 private constant BRIDGE_HEIGHT = 200;
    uint32 private constant MIN_VALIDATORS = 4;
    uint32 private constant MAX_BRIDGE_VALIDATORS = 64;
    uint64 private constant MIN_WEIGHT = 900;
    uint256 private constant MIN_EPOCH_DURATION = 1 days;
    uint256 private constant MAX_LAG = 30 days;
    uint256 private constant MAX_CERT_AGE = 1 days;
    uint256 private constant MIN_EXIT_WINDOW = 7 days;
    uint256 private constant BOOTSTRAP_WINDOW = 7 days;
    uint256 private constant MAX_DEPOSIT_RAW = 1_000_000;

    MockFinalityProver private mockProver;
    MockBridgeUSDT private token;

    function setUp() public {
        mockProver = new MockFinalityProver();
        token = new MockBridgeUSDT();
    }

    function test_BootstrapPinsOnlySuppliedPublicFixture() public {
        B3Types.SetHeader memory genesis = _set(0, 4, 1_000, 1);
        B3FinalityVerifier verifier = _verifier(genesis);

        assertTrue(verifier.initialized());
        assertEq(verifier.CHAIN_DOMAIN(), DOMAIN);
        assertEq(verifier.BOOTSTRAP_SET_HASH(), _setHash(_set(0, 4, 4, 0x44)));
        assertEq(verifier.GENESIS_SET_HASH(), _setHash(genesis));
        assertEq(verifier.PROVER_CODE_HASH(), address(mockProver).codehash);
        assertEq(verifier.MODERN_START_HEIGHT(), M);
        assertEq(verifier.BRIDGE_ACTIVATION_HEIGHT(), BRIDGE_HEIGHT);
        assertEq(verifier.MIN_BRIDGE_VALIDATORS(), MIN_VALIDATORS);
        assertEq(verifier.MAX_BRIDGE_VALIDATORS(), MAX_BRIDGE_VALIDATORS);
        assertEq(verifier.MIN_BRIDGE_TOTAL_WEIGHT(), MIN_WEIGHT);
        assertEq(verifier.MIN_EPOCH_DURATION(), MIN_EPOCH_DURATION);
        assertEq(verifier.MAX_EPOCH_LAG(), MAX_LAG);
        assertEq(verifier.MAX_CERTIFICATE_AGE(), MAX_CERT_AGE);
        assertEq(verifier.MIN_DEPOSIT_EXIT_WINDOW(), MIN_EXIT_WINDOW);
        assertEq(verifier.latestFinalizedHeight(), M - 1);
        assertEq(verifier.latestWithdrawalRoot(), bytes32(0));
        assertEq(verifier.lastRotationTime(), block.timestamp);
        assertEq(verifier.GENESIS_TIME(), block.timestamp);
    }

    function test_DelayedBootstrapHandsAuthorityToSet0ExactlyOnce() public {
        B3FinalityVerifier verifier = _uninitializedVerifierWithTiming(MAX_LAG, MAX_CERT_AGE, MIN_EXIT_WINDOW);
        B3Types.SetHeader memory genesis = _set(0, 4, 1_000, 1);
        B3Types.FinalizedBlock memory snapshot = _finalized(M - 1, bytes32(0), _setHash(genesis), 0);
        B3StakerBridge bridge =
            new B3StakerBridge(verifier, address(verifier).codehash, DOMAIN, address(token), MAX_DEPOSIT_RAW);

        assertFalse(verifier.initialized());
        assertEq(verifier.GENESIS_SET_HASH(), bytes32(0));
        assertEq(verifier.lastRotationTime(), 0);
        vm.expectRevert(B3FinalityVerifier.NotInitialized.selector);
        verifier.submitCertificate(
            _finalized(M, bytes32(0), _setHash(_set(1, 4, 1_000, 2)), 0), _set(1, 4, 1_000, 2), ""
        );
        token.mint(address(this), 10);
        token.approve(address(bridge), 10);
        assertFalse(verifier.depositViable());
        vm.expectRevert(B3StakerBridge.DepositClosed.selector);
        bridge.deposit(10, _recipient(0xB3));
        assertEq(bridge.locked(), 0);

        verifier.initialize(snapshot, genesis, "");
        assertTrue(verifier.initialized());
        assertEq(verifier.GENESIS_SET_HASH(), _setHash(genesis));
        assertEq(verifier.currentSetHash(), _setHash(genesis));
        assertEq(verifier.latestFinalizedHeight(), M - 1);
        assertEq(verifier.lastRotationTime(), block.timestamp);
        assertFalse(verifier.depositViable());

        vm.expectRevert(B3FinalityVerifier.AlreadyInitialized.selector);
        verifier.initialize(snapshot, genesis, "");
    }

    function test_DepositViabilityRejectsUninitializedBootstrapAtEveryTime() public {
        B3FinalityVerifier verifier = _uninitializedVerifierWithTiming(MAX_LAG, MAX_CERT_AGE, MIN_EXIT_WINDOW);
        B3StakerBridge bridge =
            new B3StakerBridge(verifier, address(verifier).codehash, DOMAIN, address(token), MAX_DEPOSIT_RAW);
        token.mint(address(this), 2);
        token.approve(address(bridge), 2);

        assertFalse(verifier.depositViable());
        vm.expectRevert(B3StakerBridge.DepositClosed.selector);
        bridge.deposit(1, _recipient(0xB3));
        vm.warp(verifier.BOOTSTRAP_DEADLINE());
        assertFalse(verifier.depositViable());
        vm.expectRevert(B3StakerBridge.DepositClosed.selector);
        bridge.deposit(1, _recipient(0xB4));
        assertEq(bridge.locked(), 0);
    }

    function test_DepositViabilityRejectsExpiredUninitializedBootstrap() public {
        B3FinalityVerifier verifier = _uninitializedVerifierWithTiming(MAX_LAG, MAX_CERT_AGE, MIN_EXIT_WINDOW);
        B3StakerBridge bridge =
            new B3StakerBridge(verifier, address(verifier).codehash, DOMAIN, address(token), MAX_DEPOSIT_RAW);
        token.mint(address(this), 1);
        token.approve(address(bridge), 1);

        vm.warp(verifier.BOOTSTRAP_DEADLINE() + 1);
        assertFalse(verifier.depositViable());
        vm.expectRevert(B3StakerBridge.DepositClosed.selector);
        bridge.deposit(1, _recipient(0xB3));
        assertEq(bridge.locked(), 0);
        assertEq(token.balanceOf(address(this)), 1);
    }

    function test_DepositViabilityClosesAfterRelativeLineageLag() public {
        B3FinalityVerifier verifier = _verifier(_set(0, 4, 1_000, 1));
        B3StakerBridge bridge =
            new B3StakerBridge(verifier, address(verifier).codehash, DOMAIN, address(token), MAX_DEPOSIT_RAW);
        token.mint(address(this), 2);
        token.approve(address(bridge), 2);

        _openDeposits(verifier);
        vm.warp(verifier.lastRotationTime() + MAX_LAG);
        assertFalse(verifier.depositViable());
        vm.expectRevert(B3StakerBridge.DepositClosed.selector);
        bridge.deposit(1, _recipient(0xB3));
        vm.warp(block.timestamp + 1);
        assertFalse(verifier.depositViable());
        vm.expectRevert(B3StakerBridge.DepositClosed.selector);
        bridge.deposit(1, _recipient(0xB4));
        assertEq(bridge.locked(), 0);
    }

    function test_DepositViabilityEnforcesAbsoluteEpochWindow() public {
        B3FinalityVerifier verifier = _verifier(_set(0, 4, 1_000, 1));
        B3StakerBridge bridge =
            new B3StakerBridge(verifier, address(verifier).codehash, DOMAIN, address(token), MAX_DEPOSIT_RAW);
        B3Types.SetHeader memory successor1 = _set(1, 4, 1_000, 2);
        verifier.submitCertificate(_finalized(M, bytes32(0), _setHash(successor1), 0), successor1, "");

        vm.warp(verifier.GENESIS_TIME() + MAX_LAG);
        B3Types.SetHeader memory successor2 = _set(2, 4, 1_000, 3);
        verifier.submitCertificate(_finalized(M + 1, bytes32(0), _setHash(successor2), 1), successor2, "");
        assertTrue(verifier.epochTimeValid(1));
        assertFalse(verifier.depositViable());

        token.mint(address(this), 1);
        token.approve(address(bridge), 1);
        vm.warp(verifier.GENESIS_TIME() + 2 * MAX_LAG + 1);
        assertFalse(verifier.epochTimeValid(1));
        assertFalse(verifier.depositViable());
        vm.expectRevert(B3StakerBridge.DepositClosed.selector);
        bridge.deposit(1, _recipient(0xB3));
    }

    function test_DepositViabilityRejectsWeakSuccessorSet() public {
        B3FinalityVerifier verifier = _verifier(_set(0, 4, 1_000, 1));
        B3StakerBridge bridge =
            new B3StakerBridge(verifier, address(verifier).codehash, DOMAIN, address(token), MAX_DEPOSIT_RAW);
        B3Types.SetHeader memory weakSuccessor = _set(1, 2, MIN_WEIGHT - 1, 2);
        verifier.submitCertificate(
            _finalized(BRIDGE_HEIGHT, _emptyWithdrawalRoot(), _setHash(weakSuccessor), 0), weakSuccessor, ""
        );

        assertFalse(verifier.bridgeReady());
        assertFalse(verifier.depositViable());
        token.mint(address(this), 1);
        token.approve(address(bridge), 1);
        vm.expectRevert(B3StakerBridge.DepositClosed.selector);
        bridge.deposit(1, _recipient(0xB3));
        assertEq(bridge.locked(), 0);
    }

    function test_DepositRejectsRecipientThatB3CannotMint() public {
        B3FinalityVerifier verifier = _verifier(_set(0, 4, 1_000, 1));
        B3StakerBridge bridge =
            new B3StakerBridge(verifier, address(verifier).codehash, DOMAIN, address(token), MAX_DEPOSIT_RAW);
        _openDeposits(verifier);
        token.mint(address(this), 3);
        token.approve(address(bridge), 3);

        vm.expectRevert(B3StakerBridge.BadB3Recipient.selector);
        bridge.deposit(1, bytes32(uint256(0xB3))); // missing version byte
        vm.expectRevert(B3StakerBridge.BadB3Recipient.selector);
        bridge.deposit(1, bytes32(type(uint256).max)); // nonzero padding
        vm.expectRevert(B3StakerBridge.BadB3Recipient.selector);
        bridge.deposit(1, _recipient(0)); // valid framing, unspendable key hash
        assertEq(bridge.locked(), 0);
        assertEq(token.balanceOf(address(bridge)), 0);

        bridge.deposit(1, _recipient(0xB3));
        assertEq(bridge.locked(), 1);
    }

    function test_BridgeRejectsZeroDepositCapAtConstruction() public {
        B3FinalityVerifier verifier = _verifier(_set(0, 4, 1_000, 1));
        vm.expectRevert(B3StakerBridge.BadBootstrap.selector);
        new B3StakerBridge(verifier, address(verifier).codehash, DOMAIN, address(token), 0);
    }

    function test_DepositCapAcceptsBoundaryAndRollsBackExcess() public {
        B3FinalityVerifier verifier = _verifier(_set(0, 4, 1_000, 1));
        B3StakerBridge bridge =
            new B3StakerBridge(verifier, address(verifier).codehash, DOMAIN, address(token), MAX_DEPOSIT_RAW);
        _openDeposits(verifier);
        token.mint(address(this), MAX_DEPOSIT_RAW * 2 + 1);
        token.approve(address(bridge), MAX_DEPOSIT_RAW * 2 + 1);

        bridge.deposit(MAX_DEPOSIT_RAW, _recipient(0xB3));
        assertEq(bridge.MAX_DEPOSIT_RAW(), MAX_DEPOSIT_RAW);
        assertEq(bridge.locked(), MAX_DEPOSIT_RAW);
        assertEq(bridge.nextDepositId(), 1);

        vm.expectRevert(B3StakerBridge.DepositCapExceeded.selector);
        bridge.deposit(MAX_DEPOSIT_RAW + 1, _recipient(0xB4));
        assertEq(bridge.locked(), MAX_DEPOSIT_RAW);
        assertEq(bridge.nextDepositId(), 1);
        assertEq(token.balanceOf(address(bridge)), MAX_DEPOSIT_RAW);
    }

    function test_DepositCapUsesFeeTokenActualReceivedAmount() public {
        B3FinalityVerifier verifier = _verifier(_set(0, 4, 1_000, 1));
        FeeBridgeToken feeToken = new FeeBridgeToken();
        B3StakerBridge bridge = new B3StakerBridge(verifier, address(verifier).codehash, DOMAIN, address(feeToken), 990);
        _openDeposits(verifier);
        feeToken.mint(address(this), 2_001);
        feeToken.approve(address(bridge), 2_001);

        bridge.deposit(1_000, _recipient(0xB3));
        assertEq(bridge.locked(), 990);
        assertEq(feeToken.balanceOf(address(bridge)), 990);

        vm.expectRevert(B3StakerBridge.DepositCapExceeded.selector);
        bridge.deposit(1_001, _recipient(0xB4)); // actual received would be 991
        assertEq(bridge.locked(), 990);
        assertEq(bridge.nextDepositId(), 1);
        assertEq(feeToken.balanceOf(address(bridge)), 990);
    }

    function test_DelayedBootstrapRejectsBadProofAndExpiry() public {
        B3FinalityVerifier verifier = _uninitializedVerifierWithTiming(MAX_LAG, MAX_CERT_AGE, MIN_EXIT_WINDOW);
        B3Types.SetHeader memory genesis = _set(0, 4, 1_000, 1);
        B3Types.FinalizedBlock memory snapshot = _finalized(M - 1, bytes32(0), _setHash(genesis), 0);

        mockProver.setResult(false);
        vm.expectRevert(B3FinalityVerifier.ProofRejected.selector);
        verifier.initialize(snapshot, genesis, "");
        mockProver.setResult(true);

        vm.warp(verifier.BOOTSTRAP_DEADLINE() + 1);
        vm.expectRevert(B3FinalityVerifier.BootstrapExpired.selector);
        verifier.initialize(snapshot, genesis, "");
    }

    function test_BootstrapRejectsUnpinnedSnapshotButTracksWeakSet() public {
        B3Types.SetHeader memory genesis = _set(0, 4, 1_000, 1);
        B3Types.SetHeader memory bootstrap = _set(0, 4, 4, 0x44);

        vm.expectRevert(B3FinalityVerifier.BadGenesisSet.selector);
        new B3FinalityVerifier(
            DOMAIN,
            bootstrap,
            bytes32(uint256(0xBAD)),
            mockProver,
            address(mockProver).codehash,
            _deploymentConfig(MAX_BRIDGE_VALIDATORS, MAX_LAG, MAX_CERT_AGE, MIN_EXIT_WINDOW)
        );

        // Chain lineage may begin with two validators. That public set is
        // tracked, while bridgeReady remains false until a qualifying set
        // actually signs a post-activation certificate.
        genesis = _set(0, 2, MIN_WEIGHT - 1, 9);
        B3FinalityVerifier verifier = _verifier(genesis);
        assertEq(verifier.currentEpoch(), 0);
        assertFalse(verifier.bridgeReady());
    }

    function test_BootstrapRejectsUnpinnedProverRuntime() public {
        B3Types.SetHeader memory bootstrap = _set(0, 4, 4, 0x44);
        vm.expectRevert(B3FinalityVerifier.BadProver.selector);
        new B3FinalityVerifier(
            DOMAIN,
            bootstrap,
            _setHash(bootstrap),
            mockProver,
            bytes32(uint256(1)),
            _deploymentConfig(MAX_BRIDGE_VALIDATORS, MAX_LAG, MAX_CERT_AGE, MIN_EXIT_WINDOW)
        );
    }

    function test_BootstrapRejectsUnbenchmarkedValidatorLimit() public {
        B3Types.SetHeader memory bootstrap = _set(0, 4, 4, 0x44);
        vm.expectRevert(B3FinalityVerifier.BadBootstrap.selector);
        new B3FinalityVerifier(
            DOMAIN,
            bootstrap,
            _setHash(bootstrap),
            mockProver,
            address(mockProver).codehash,
            _deploymentConfig(B3Types.MAX_PROVEN_BRIDGE_VALIDATORS + 1, MAX_LAG, MAX_CERT_AGE, MIN_EXIT_WINDOW)
        );
    }

    function test_BootstrapRejectsSubdayEpochDuration() public {
        B3Types.SetHeader memory bootstrap = _set(0, 4, 4, 0x44);
        B3FinalityVerifier.DeploymentConfig memory config =
            _deploymentConfig(MAX_BRIDGE_VALIDATORS, MAX_LAG, MAX_CERT_AGE, MIN_EXIT_WINDOW);
        config.minEpochDuration = B3Types.MIN_PROVEN_EPOCH_DURATION - 1;

        vm.expectRevert(B3FinalityVerifier.BadBootstrap.selector);
        new B3FinalityVerifier(DOMAIN, bootstrap, _setHash(bootstrap), mockProver, address(mockProver).codehash, config);
    }

    function test_BootstrapRejectsUnsafeFreshnessPins() public {
        vm.expectRevert(B3FinalityVerifier.BadBootstrap.selector);
        _uninitializedVerifierWithTiming(MAX_LAG, 0, MIN_EXIT_WINDOW);

        // Certificate freshness must be strictly shorter than the complete
        // validator-set lifetime.
        vm.expectRevert(B3FinalityVerifier.BadBootstrap.selector);
        _uninitializedVerifierWithTiming(MAX_LAG, MAX_LAG, MIN_EXIT_WINDOW);

        vm.expectRevert(B3FinalityVerifier.BadBootstrap.selector);
        _uninitializedVerifierWithTiming(MAX_LAG, MAX_CERT_AGE, 0);

        vm.expectRevert(B3FinalityVerifier.BadBootstrap.selector);
        _uninitializedVerifierWithTiming(MAX_LAG, MAX_CERT_AGE, MAX_LAG);

        vm.expectRevert(B3FinalityVerifier.BadBootstrap.selector);
        _uninitializedVerifierWithTiming(MAX_LAG, MAX_CERT_AGE, MAX_LAG + 1);
    }

    function test_NonzeroWithdrawalRootIsDormantBeforeBridgeHeight() public {
        B3Types.SetHeader memory genesis = _set(0, 4, 1_000, 1);
        B3FinalityVerifier verifier = _verifier(genesis);
        B3Types.SetHeader memory successor = _set(1, 4, 1_000, 2);
        B3Types.FinalizedBlock memory finalizedBlock = _finalized(M, bytes32(uint256(1)), _setHash(successor), 0);

        vm.expectRevert(B3FinalityVerifier.PrematureWithdrawalRoot.selector);
        verifier.submitCertificate(finalizedBlock, successor, "");
    }

    function test_ZeroWithdrawalRootRejectedAtBridgeHeight() public {
        B3FinalityVerifier verifier = _verifier(_set(0, 4, 1_000, 1));
        B3Types.SetHeader memory successor = _set(1, 4, 1_000, 2);

        vm.expectRevert(B3FinalityVerifier.MissingWithdrawalRoot.selector);
        verifier.submitCertificate(_finalized(BRIDGE_HEIGHT, bytes32(0), _setHash(successor), 0), successor, "");
    }

    function test_WeakSetTracksLineageButCannotTakeBridgeAuthority() public {
        B3Types.SetHeader memory genesis = _set(0, 2, MIN_WEIGHT - 1, 1);
        B3FinalityVerifier verifier = _verifier(genesis);
        B3Types.SetHeader memory successor = _set(1, 4, 1_000, 2);
        B3Types.FinalizedBlock memory finalizedBlock = _finalized(M, bytes32(0), _setHash(successor), 0);

        verifier.submitCertificate(finalizedBlock, successor, "");
        assertEq(verifier.latestFinalizedHeight(), M);
        assertFalse(verifier.bridgeReady());

        // The qualifying successor becomes authoritative only when it signs
        // its own epoch-1 certificate.
        vm.warp(verifier.GENESIS_TIME() + MIN_EPOCH_DURATION);
        B3Types.SetHeader memory successor2 = _set(2, 4, 1_000, 3);
        verifier.submitCertificate(
            _finalized(BRIDGE_HEIGHT, bytes32(uint256(7)), _setHash(successor2), 1), successor2, ""
        );
        assertTrue(verifier.bridgeReady());
        assertEq(verifier.latestBridgeWithdrawalRoot(), bytes32(uint256(7)));
    }

    function test_StrongKnownNextSetOpensDeposits() public {
        B3FinalityVerifier verifier = _verifier(_set(0, 4, 1_000, 1));
        B3StakerBridge bridge =
            new B3StakerBridge(verifier, address(verifier).codehash, ASSET, address(token), MAX_DEPOSIT_RAW);
        B3Types.SetHeader memory successor = _set(1, 4, 1_000, 2);

        verifier.submitCertificate(
            _finalized(BRIDGE_HEIGHT, _emptyWithdrawalRoot(), _setHash(successor), 0), successor, ""
        );

        assertEq(verifier.nextSetHash(), _setHash(successor));
        assertTrue(verifier.bridgeReady());
        assertTrue(verifier.depositViable());
        assertTrue(verifier.releaseReady());
        token.mint(address(this), 1);
        token.approve(address(bridge), 1);
        bridge.deposit(1, _recipient(1));
        assertEq(bridge.locked(), 1);
    }

    function test_OversizedKnownNextSetClosesBridgeReadiness() public {
        B3FinalityVerifier verifier = _verifier(_set(0, 4, 1_000, 1));
        B3Types.SetHeader memory oversized = _set(1, MAX_BRIDGE_VALIDATORS + 1, 10_000, 2);

        verifier.submitCertificate(
            _finalized(BRIDGE_HEIGHT, _emptyWithdrawalRoot(), _setHash(oversized), 0), oversized, ""
        );

        assertEq(verifier.nextSetHash(), _setHash(oversized));
        assertFalse(verifier.bridgeReady());
        assertTrue(verifier.releaseReady());
    }

    function test_WeakKnownNextSetClosesNewDepositsButKeepsFinalizedRelease() public {
        B3FinalityVerifier verifier = _verifier(_set(0, 4, 1_000, 1));
        B3StakerBridge bridge =
            new B3StakerBridge(verifier, address(verifier).codehash, ASSET, address(token), MAX_DEPOSIT_RAW);
        B3Types.SetHeader memory strongSuccessor = _set(1, 4, 1_000, 2);

        verifier.submitCertificate(
            _finalized(BRIDGE_HEIGHT, _emptyWithdrawalRoot(), _setHash(strongSuccessor), 0), strongSuccessor, ""
        );
        assertTrue(verifier.bridgeReady());

        token.mint(address(this), 1_000_010);
        token.approve(address(bridge), 1_000_010);
        bridge.deposit(1_000_000, _recipient(0xB3));

        B3StakerBridge.Withdrawal memory withdrawal = B3StakerBridge.Withdrawal({
            withdrawalId: 0, recipient: address(0xBEEF), amount: 250_000, b3Height: BRIDGE_HEIGHT
        });
        (bytes32 root, bytes32[32] memory path) = _withdrawalRoot(bridge, withdrawal);
        B3Types.SetHeader memory weakSuccessor = _set(2, 2, MIN_WEIGHT - 1, 3);

        // The strong epoch-1 set may finalize this root and attest the weak
        // epoch-2 set. Existing finalized releases remain live, but new custody
        // closes before the bridge-ineligible handover.
        vm.warp(verifier.GENESIS_TIME() + MIN_EPOCH_DURATION);
        verifier.submitCertificate(_finalized(BRIDGE_HEIGHT + 10, root, _setHash(weakSuccessor), 1), weakSuccessor, "");

        assertEq(verifier.currentEpoch(), 1);
        assertEq(verifier.nextSetHash(), _setHash(weakSuccessor));
        assertFalse(verifier.bridgeReady());
        assertFalse(verifier.depositViable());
        assertTrue(verifier.releaseReady());

        vm.expectRevert(B3StakerBridge.DepositClosed.selector);
        bridge.deposit(10, _recipient(0xB4));
        assertEq(bridge.locked(), 1_000_000);

        bridge.release(withdrawal, path);
        assertEq(token.balanceOf(address(0xBEEF)), 250_000);
        assertEq(bridge.locked(), 750_000);
        assertTrue(bridge.released(0));
    }

    function test_FinalizedBurnProofReleasesCanonicalTokenOnce() public {
        B3Types.SetHeader memory genesis = _set(0, 4, 1_000, 1);
        B3FinalityVerifier verifier = _verifier(genesis);
        B3StakerBridge bridge =
            new B3StakerBridge(verifier, address(verifier).codehash, ASSET, address(token), MAX_DEPOSIT_RAW);

        B3StakerBridge.Withdrawal memory withdrawal = B3StakerBridge.Withdrawal({
            withdrawalId: 0, recipient: address(0xBEEF), amount: 250_000, b3Height: BRIDGE_HEIGHT
        });
        B3Types.SetHeader memory successor = _set(1, 4, 1_000, 2);
        // A threshold-qualified post-activation certificate opens deposits.
        verifier.submitCertificate(
            _finalized(BRIDGE_HEIGHT, _emptyWithdrawalRoot(), _setHash(successor), 0), successor, ""
        );
        token.mint(address(this), 1_000_000);
        token.approve(address(bridge), 1_000_000);
        bridge.deposit(1_000_000, _recipient(0xB3));
        assertEq(bridge.locked(), 1_000_000);

        (bytes32 root, bytes32[32] memory path) = _withdrawalRoot(bridge, withdrawal);
        verifier.submitCertificate(_finalized(BRIDGE_HEIGHT + 10, root, _setHash(successor), 0), successor, "");

        bridge.release(withdrawal, path);
        assertEq(token.balanceOf(address(0xBEEF)), 250_000);
        assertEq(bridge.locked(), 750_000);
        assertTrue(bridge.released(0));

        vm.expectRevert(B3StakerBridge.AlreadyReleased.selector);
        bridge.release(withdrawal, path);
    }

    function test_HostileTokenCannotReenterRelease() public {
        B3FinalityVerifier verifier = _verifier(_set(0, 4, 1_000, 1));
        HostileBridgeToken hostile = new HostileBridgeToken();
        B3StakerBridge bridge =
            new B3StakerBridge(verifier, address(verifier).codehash, ASSET, address(hostile), MAX_DEPOSIT_RAW);
        B3Types.SetHeader memory successor = _set(1, 4, 1_000, 2);
        verifier.submitCertificate(
            _finalized(BRIDGE_HEIGHT, _emptyWithdrawalRoot(), _setHash(successor), 0), successor, ""
        );

        hostile.mint(address(this), 1_000_000);
        hostile.approve(address(bridge), 1_000_000);
        bridge.deposit(1_000_000, _recipient(0xB3));

        B3StakerBridge.Withdrawal memory withdrawal = B3StakerBridge.Withdrawal({
            withdrawalId: 0, recipient: address(0xBEEF), amount: 250_000, b3Height: BRIDGE_HEIGHT
        });
        (bytes32 root, bytes32[32] memory path) = _withdrawalRoot(bridge, withdrawal);
        verifier.submitCertificate(_finalized(BRIDGE_HEIGHT + 10, root, _setHash(successor), 0), successor, "");

        hostile.armCallback(address(bridge), abi.encodeWithSelector(B3StakerBridge.release.selector, withdrawal, path));
        bridge.release(withdrawal, path);

        assertFalse(hostile.callbackSucceeded());
        assertEq(uint256(uint32(hostile.callbackError())), uint256(uint32(B3StakerBridge.Reentrancy.selector)));
        assertEq(hostile.balanceOf(address(0xBEEF)), 250_000);
        assertEq(bridge.locked(), 750_000);
        assertTrue(bridge.released(0));
    }

    function test_ReleaseTransferFailureRollsBackAndCanRetry() public {
        B3FinalityVerifier verifier = _verifier(_set(0, 4, 1_000, 1));
        HostileBridgeToken hostile = new HostileBridgeToken();
        B3StakerBridge bridge =
            new B3StakerBridge(verifier, address(verifier).codehash, ASSET, address(hostile), MAX_DEPOSIT_RAW);
        B3Types.SetHeader memory successor = _set(1, 4, 1_000, 2);
        verifier.submitCertificate(
            _finalized(BRIDGE_HEIGHT, _emptyWithdrawalRoot(), _setHash(successor), 0), successor, ""
        );

        hostile.mint(address(this), 1_000_000);
        hostile.approve(address(bridge), 1_000_000);
        bridge.deposit(1_000_000, _recipient(0xB3));

        B3StakerBridge.Withdrawal memory withdrawal = B3StakerBridge.Withdrawal({
            withdrawalId: 0, recipient: address(0xBEEF), amount: 250_000, b3Height: BRIDGE_HEIGHT
        });
        (bytes32 root, bytes32[32] memory path) = _withdrawalRoot(bridge, withdrawal);
        verifier.submitCertificate(_finalized(BRIDGE_HEIGHT + 10, root, _setHash(successor), 0), successor, "");

        hostile.setTransferMode(HostileBridgeToken.TransferMode.RETURN_FALSE);
        vm.expectRevert(B3StakerBridge.TransferFailed.selector);
        bridge.release(withdrawal, path);
        assertFalse(bridge.released(0));
        assertEq(bridge.locked(), 1_000_000);
        assertEq(hostile.balanceOf(address(0xBEEF)), 0);

        hostile.setTransferMode(HostileBridgeToken.TransferMode.REVERT);
        vm.expectRevert(B3StakerBridge.TransferFailed.selector);
        bridge.release(withdrawal, path);
        assertFalse(bridge.released(0));
        assertEq(bridge.locked(), 1_000_000);
        assertEq(hostile.balanceOf(address(0xBEEF)), 0);

        hostile.setTransferMode(HostileBridgeToken.TransferMode.SUCCEED);
        bridge.release(withdrawal, path);
        assertTrue(bridge.released(0));
        assertEq(bridge.locked(), 750_000);
        assertEq(hostile.balanceOf(address(0xBEEF)), 250_000);
    }

    function test_NoCertificateClosesDepositsAndRelease() public {
        B3FinalityVerifier verifier = _verifier(_set(0, 4, 1_000, 1));
        B3StakerBridge bridge =
            new B3StakerBridge(verifier, address(verifier).codehash, ASSET, address(token), MAX_DEPOSIT_RAW);
        B3StakerBridge.Withdrawal memory withdrawal = B3StakerBridge.Withdrawal({
            withdrawalId: 0, recipient: address(0xBEEF), amount: 1, b3Height: BRIDGE_HEIGHT
        });
        (, bytes32[32] memory path) = _withdrawalRoot(bridge, withdrawal);

        vm.expectRevert(B3StakerBridge.BridgeNotReady.selector);
        bridge.release(withdrawal, path);

        token.mint(address(this), 10);
        token.approve(address(bridge), 10);
        assertFalse(verifier.depositViable());
        vm.expectRevert(B3StakerBridge.DepositClosed.selector);
        bridge.deposit(10, _recipient(1));
        assertEq(bridge.locked(), 0);
    }

    function test_FirstCertificateRejectedAfterBootstrapExpires() public {
        B3FinalityVerifier verifier = _verifier(_set(0, 4, 1_000, 1));
        uint256 deploymentTime = verifier.lastRotationTime();
        assertEq(verifier.lastCertificateTime(), 0);
        vm.warp(deploymentTime + MAX_LAG + 1);
        B3Types.SetHeader memory successor = _set(1, 4, 1_000, 2);

        vm.expectRevert(B3FinalityVerifier.EpochLagExceeded.selector);
        verifier.submitCertificate(_finalized(M, bytes32(0), _setHash(successor), 0), successor, "");

        assertEq(verifier.latestFinalizedHeight(), M - 1);
        assertEq(verifier.lastCertificateTime(), 0);
        assertEq(verifier.lastRotationTime(), deploymentTime);
    }

    function test_EpochCannotAdvanceBeforeMinimumDuration() public {
        B3FinalityVerifier verifier = _verifier(_set(0, 4, 1_000, 1));
        B3Types.SetHeader memory successor = _set(1, 4, 1_000, 2);
        verifier.submitCertificate(_finalized(M, bytes32(0), _setHash(successor), 0), successor, "");
        B3Types.SetHeader memory successor2 = _set(2, 4, 1_000, 3);

        vm.expectRevert(B3FinalityVerifier.EpochTimeWindow.selector);
        verifier.submitCertificate(_finalized(M + 10, bytes32(0), _setHash(successor2), 1), successor2, "");

        vm.warp(verifier.GENESIS_TIME() + MIN_EPOCH_DURATION);
        verifier.submitCertificate(_finalized(M + 10, bytes32(0), _setHash(successor2), 1), successor2, "");
        assertEq(verifier.currentEpoch(), 1);
    }

    function test_DelayedLineageCannotBatchWalkPastEpochs() public {
        B3FinalityVerifier verifier = _verifier(_set(0, 4, 1_000, 1));
        B3Types.SetHeader memory successor1 = _set(1, 4, 1_000, 2);
        verifier.submitCertificate(_finalized(M, bytes32(0), _setHash(successor1), 0), successor1, "");

        // Epoch 1's absolute lower bound is long past, so its first handover
        // may catch up. That handover starts a fresh minimum-duration clock.
        vm.warp(verifier.GENESIS_TIME() + 29 * MIN_EPOCH_DURATION);
        B3Types.SetHeader memory successor2 = _set(2, 4, 1_000, 3);
        verifier.submitCertificate(_finalized(M + 10, bytes32(0), _setHash(successor2), 1), successor2, "");

        // Epoch 2's absolute lower bound is also in the past, but it cannot
        // be replayed immediately after the epoch-1 handover.
        B3Types.SetHeader memory successor3 = _set(3, 4, 1_000, 4);
        vm.expectRevert(B3FinalityVerifier.EpochTimeWindow.selector);
        verifier.submitCertificate(_finalized(M + 20, bytes32(0), _setHash(successor3), 2), successor3, "");

        vm.warp(block.timestamp + MIN_EPOCH_DURATION);
        verifier.submitCertificate(_finalized(M + 20, bytes32(0), _setHash(successor3), 2), successor3, "");
        assertEq(verifier.currentEpoch(), 2);
    }

    function test_EpochZeroExpiresAtAbsoluteMaximum() public {
        B3FinalityVerifier verifier = _verifier(_set(0, 4, 1_000, 1));
        B3Types.SetHeader memory successor = _set(1, 4, 1_000, 2);

        vm.warp(verifier.GENESIS_TIME() + MAX_LAG + 1);
        assertFalse(verifier.epochTimeValid(0));
        vm.expectRevert(B3FinalityVerifier.EpochLagExceeded.selector);
        verifier.submitCertificate(_finalized(M, bytes32(0), _setHash(successor), 0), successor, "");
    }

    function test_SameEpochCertificateCannotExtendSetLifetime() public {
        B3FinalityVerifier verifier = _verifier(_set(0, 4, 1_000, 1));
        B3Types.SetHeader memory successor = _set(1, 4, 1_000, 2);
        verifier.submitCertificate(_finalized(M, bytes32(0), _setHash(successor), 0), successor, "");
        uint256 rotationStart = verifier.lastRotationTime();

        // A current-epoch checkpoint remains useful, but it is not a validator
        // set handover and therefore must not renew the set's lifetime.
        vm.warp(rotationStart + 20 days);
        verifier.submitCertificate(_finalized(M + 10, bytes32(0), _setHash(successor), 0), successor, "");
        assertEq(verifier.lastCertificateTime(), block.timestamp);
        assertEq(verifier.lastRotationTime(), rotationStart);

        // This is only ten days after the keepalive above, but more than the
        // permitted lifetime of the still-unrotated signing set.
        vm.warp(rotationStart + 30 days + 1);
        vm.expectRevert(B3FinalityVerifier.EpochLagExceeded.selector);
        verifier.submitCertificate(_finalized(M + 20, bytes32(0), _setHash(successor), 0), successor, "");
    }

    function test_BridgeReadyCertificateAgeBoundary() public {
        B3FinalityVerifier verifier = _verifier(_set(0, 4, 1_000, 1));
        B3Types.SetHeader memory successor = _set(1, 4, 1_000, 2);
        verifier.submitCertificate(
            _finalized(BRIDGE_HEIGHT, _emptyWithdrawalRoot(), _setHash(successor), 0), successor, ""
        );
        uint256 certificateTime = verifier.lastCertificateTime();

        vm.warp(certificateTime + MAX_CERT_AGE);
        assertTrue(verifier.bridgeReady());
        vm.warp(block.timestamp + 1);
        assertFalse(verifier.bridgeReady());
        assertTrue(verifier.releaseReady());
    }

    function test_BridgeReadyDepositExitWindowBoundary() public {
        B3FinalityVerifier verifier = _verifier(_set(0, 4, 1_000, 1));
        B3Types.SetHeader memory successor = _set(1, 4, 1_000, 2);
        verifier.submitCertificate(
            _finalized(BRIDGE_HEIGHT, _emptyWithdrawalRoot(), _setHash(successor), 0), successor, ""
        );
        uint256 rotationStart = verifier.lastRotationTime();

        // Refresh finality one second before the configured exit cutoff.
        // This must not refresh the validator-set lifetime itself.
        vm.warp(rotationStart + MAX_LAG - MIN_EXIT_WINDOW - 1);
        verifier.submitCertificate(
            _finalized(BRIDGE_HEIGHT + 10, _emptyWithdrawalRoot(), _setHash(successor), 0), successor, ""
        );
        assertEq(verifier.lastRotationTime(), rotationStart);
        assertEq(verifier.lastCertificateTime(), block.timestamp);
        assertTrue(verifier.bridgeReady());

        vm.warp(block.timestamp + 1);
        assertFalse(verifier.bridgeReady());
        assertTrue(verifier.releaseReady());
    }

    function test_DepositClosesAtExitWindowCutoff() public {
        B3FinalityVerifier verifier = _verifier(_set(0, 4, 1_000, 1));
        B3StakerBridge bridge =
            new B3StakerBridge(verifier, address(verifier).codehash, ASSET, address(token), MAX_DEPOSIT_RAW);
        B3Types.SetHeader memory successor = _set(1, 4, 1_000, 2);
        verifier.submitCertificate(
            _finalized(BRIDGE_HEIGHT, _emptyWithdrawalRoot(), _setHash(successor), 0), successor, ""
        );
        uint256 rotationStart = verifier.lastRotationTime();

        token.mint(address(this), 10);
        token.approve(address(bridge), 10);

        vm.warp(rotationStart + MAX_LAG - MIN_EXIT_WINDOW - 1);
        verifier.submitCertificate(
            _finalized(BRIDGE_HEIGHT + 10, _emptyWithdrawalRoot(), _setHash(successor), 0), successor, ""
        );
        assertTrue(verifier.bridgeReady());

        vm.warp(block.timestamp + 1);
        assertFalse(verifier.depositViable());
        vm.expectRevert(B3StakerBridge.DepositClosed.selector);
        bridge.deposit(10, _recipient(1));
        assertEq(bridge.locked(), 0);
        assertEq(token.balanceOf(address(this)), 10);
    }

    function test_PrecommittedNextSetCanRotateAtLagBoundary() public {
        B3FinalityVerifier verifier = _verifier(_set(0, 4, 1_000, 1));
        B3Types.SetHeader memory successor = _set(1, 4, 1_000, 2);
        verifier.submitCertificate(_finalized(M, bytes32(0), _setHash(successor), 0), successor, "");
        uint256 rotationStart = verifier.lastRotationTime();

        vm.warp(rotationStart + 30 days);
        B3Types.SetHeader memory successor2 = _set(2, 4, 1_000, 3);
        verifier.submitCertificate(_finalized(M + 10, bytes32(0), _setHash(successor2), 1), successor2, "");

        assertEq(verifier.currentEpoch(), 1);
        assertEq(verifier.lastRotationTime(), block.timestamp);
    }

    function test_PrecommittedNextSetCannotRecoverAfterLag() public {
        B3FinalityVerifier verifier = _verifier(_set(0, 4, 1_000, 1));
        B3Types.SetHeader memory successor = _set(1, 4, 1_000, 2);
        verifier.submitCertificate(_finalized(M, bytes32(0), _setHash(successor), 0), successor, "");
        vm.warp(verifier.lastRotationTime() + 30 days + 1);

        // The successor header was authenticated before expiry, but its later
        // signature has no trusted signing time. Accepting it now would let an
        // obsolete set reopen the verifier after the weak-subjectivity bound.
        B3Types.SetHeader memory successor2 = _set(2, 4, 1_000, 3);
        vm.expectRevert(B3FinalityVerifier.EpochLagExceeded.selector);
        verifier.submitCertificate(_finalized(M + 10, bytes32(0), _setHash(successor2), 1), successor2, "");
    }

    function test_StaleVerifierClosesBridgeReadiness() public {
        B3FinalityVerifier verifier = _verifier(_set(0, 4, 1_000, 1));
        B3StakerBridge bridge =
            new B3StakerBridge(verifier, address(verifier).codehash, ASSET, address(token), MAX_DEPOSIT_RAW);
        B3Types.SetHeader memory successor = _set(1, 4, 1_000, 2);
        verifier.submitCertificate(
            _finalized(BRIDGE_HEIGHT, bytes32(uint256(7)), _setHash(successor), 0), successor, ""
        );
        uint256 rotationStart = verifier.lastRotationTime();
        uint64 frozenHeight = verifier.latestBridgeFinalizedHeight();
        bytes32 frozenRoot = verifier.latestBridgeWithdrawalRoot();
        assertTrue(verifier.bridgeReady());

        vm.warp(block.timestamp + MAX_CERT_AGE);
        assertTrue(verifier.bridgeReady());
        vm.warp(block.timestamp + 1);
        assertFalse(verifier.bridgeReady());
        assertTrue(verifier.releaseReady());

        token.mint(address(this), 1);
        token.approve(address(bridge), 1);
        assertFalse(verifier.depositViable());
        vm.expectRevert(B3StakerBridge.DepositClosed.selector);
        bridge.deposit(1, _recipient(1));
        assertEq(bridge.locked(), 0);
        B3StakerBridge.Withdrawal memory withdrawal = B3StakerBridge.Withdrawal({
            withdrawalId: 0, recipient: address(0xBEEF), amount: 1, b3Height: BRIDGE_HEIGHT
        });
        bytes32[32] memory path;
        vm.expectRevert(B3StakerBridge.BadWithdrawalProof.selector);
        bridge.release(withdrawal, path);

        vm.warp(rotationStart + MAX_LAG + 1);
        vm.expectRevert(B3FinalityVerifier.EpochLagExceeded.selector);
        verifier.submitCertificate(
            _finalized(BRIDGE_HEIGHT + 10, bytes32(uint256(8)), _setHash(successor), 0), successor, ""
        );
        assertEq(verifier.latestBridgeFinalizedHeight(), frozenHeight);
        assertEq(verifier.latestBridgeWithdrawalRoot(), frozenRoot);
    }

    function test_StaleVerifierReleasesAlreadyFinalizedWithdrawal() public {
        B3Types.SetHeader memory genesis = _set(0, 4, 1_000, 1);
        B3FinalityVerifier verifier = _verifier(genesis);
        B3StakerBridge bridge =
            new B3StakerBridge(verifier, address(verifier).codehash, ASSET, address(token), MAX_DEPOSIT_RAW);
        B3Types.SetHeader memory successor = _set(1, 4, 1_000, 2);

        // A fresh qualified certificate opens deposits.
        verifier.submitCertificate(
            _finalized(BRIDGE_HEIGHT, _emptyWithdrawalRoot(), _setHash(successor), 0), successor, ""
        );
        uint256 rotationStart = verifier.lastRotationTime();
        token.mint(address(this), 1_000_000);
        token.approve(address(bridge), 1_000_000);
        bridge.deposit(1_000_000, _recipient(0xB3));

        B3StakerBridge.Withdrawal memory withdrawal = B3StakerBridge.Withdrawal({
            withdrawalId: 0, recipient: address(0xBEEF), amount: 250_000, b3Height: BRIDGE_HEIGHT
        });
        (bytes32 root, bytes32[32] memory path) = _withdrawalRoot(bridge, withdrawal);
        verifier.submitCertificate(_finalized(BRIDGE_HEIGHT + 10, root, _setHash(successor), 0), successor, "");

        vm.warp(rotationStart + 30 days + 1);
        assertFalse(verifier.bridgeReady());
        assertTrue(verifier.releaseReady());

        bridge.release(withdrawal, path);
        assertEq(token.balanceOf(address(0xBEEF)), 250_000);
        assertEq(bridge.locked(), 750_000);
        assertTrue(bridge.released(0));
    }

    function test_OrderedValidatorPathAndGeneratorArePinned() public {
        BlsCertificateProver prover = new BlsCertificateProver();
        bytes memory pubkey = _publicKey(7);
        bytes32[13] memory path;
        for (uint256 i = 0; i < path.length; ++i) {
            path[i] = keccak256(abi.encodePacked("sibling", i));
        }

        uint32 index = 5;
        bytes32 expected = keccak256(abi.encodePacked(index, pubkey, uint64(77)));
        for (uint256 i = 0; i < path.length; ++i) {
            expected = ((uint256(index) >> i) & 1) == 0
                ? keccak256(abi.encodePacked(expected, path[i]))
                : keccak256(abi.encodePacked(path[i], expected));
        }
        assertEq(prover.validatorMemberRoot(index, pubkey, 77, path), expected);
        assertEq(prover.negativeGenerator().length, 128);
    }

    function test_ProverRequiresHeadcountAsWellAsWeightQuorum() public {
        BlsCertificateProver prover = new BlsCertificateProver();
        assertEq(prover.headcountQuorum(2), 2);
        assertEq(prover.headcountQuorum(4), 3);
        assertEq(prover.headcountQuorum(5), 4);

        B3Types.SetHeader memory set = _set(0, 4, 1_000, 1);
        B3Types.FinalizedBlock memory finalizedBlock;
        bytes memory bitmap = hex"01"; // one signer, even if it held >2/3 weight
        bytes memory signature = new bytes(256);
        signature[0] = bytes1(uint8(1));
        bytes memory aggregatePubkey = new bytes(128);
        BlsCertificateProver.Absent[] memory absent = new BlsCertificateProver.Absent[](3);
        bytes memory proof = abi.encode(bitmap, signature, aggregatePubkey, absent);
        assertFalse(prover.verify(DOMAIN, finalizedBlock, _setHash(set), set, proof));
    }

    function test_WithdrawalLeafMatchesB3CrossLanguageVector() public pure {
        bytes32 asset = hex"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
        address originToken = address(bytes20(hex"202122232425262728292a2b2c2d2e2f30313233"));
        address recipient = address(bytes20(hex"404142434445464748494a4b4c4d4e4f50515253"));
        bytes32 leaf = keccak256(
            abi.encodePacked(uint64(0), uint64(1), asset, originToken, recipient, uint256(1_000_000), uint64(815_000))
        );
        require(
            leaf == hex"f96ee37321b191d9ba3e573fd7739ab8a163033824a1c534045bd168c3c88b44", "B3 withdrawal leaf vector"
        );
    }

    function test_ProverRejectsWrongSetHashBeforeDecodingWitness() public {
        BlsCertificateProver prover = new BlsCertificateProver();
        B3Types.SetHeader memory set = _set(0, 4, 1_000, 1);
        B3Types.FinalizedBlock memory finalizedBlock;
        assertFalse(prover.verify(DOMAIN, finalizedBlock, bytes32(uint256(0xBAD)), set, ""));
    }

    function _verifier(B3Types.SetHeader memory genesis) private returns (B3FinalityVerifier) {
        B3FinalityVerifier verifier = _uninitializedVerifierWithTiming(MAX_LAG, MAX_CERT_AGE, MIN_EXIT_WINDOW);
        verifier.initialize(_finalized(M - 1, bytes32(0), _setHash(genesis), 0), genesis, "");
        return verifier;
    }

    function _openDeposits(B3FinalityVerifier verifier) private {
        B3Types.SetHeader memory successor = _set(1, 4, 1_000, 2);
        verifier.submitCertificate(
            _finalized(BRIDGE_HEIGHT, _emptyWithdrawalRoot(), _setHash(successor), 0), successor, ""
        );
        assertTrue(verifier.depositViable());
    }

    function _uninitializedVerifierWithTiming(uint256 maxLag, uint256 maxCertificateAge, uint256 minExitWindow)
        private
        returns (B3FinalityVerifier)
    {
        B3Types.SetHeader memory bootstrap = _set(0, 4, 4, 0x44);
        return new B3FinalityVerifier(
            DOMAIN,
            bootstrap,
            _setHash(bootstrap),
            mockProver,
            address(mockProver).codehash,
            _deploymentConfig(MAX_BRIDGE_VALIDATORS, maxLag, maxCertificateAge, minExitWindow)
        );
    }

    function _deploymentConfig(
        uint32 maxBridgeValidators,
        uint256 maxLag,
        uint256 maxCertificateAge,
        uint256 minExitWindow
    ) private view returns (B3FinalityVerifier.DeploymentConfig memory) {
        return B3FinalityVerifier.DeploymentConfig({
            modernStartHeight: M,
            bridgeActivationHeight: BRIDGE_HEIGHT,
            minBridgeValidators: MIN_VALIDATORS,
            maxBridgeValidators: maxBridgeValidators,
            minBridgeTotalWeight: MIN_WEIGHT,
            minEpochDuration: MIN_EPOCH_DURATION,
            maxEpochLag: maxLag,
            maxCertificateAge: maxCertificateAge,
            minDepositExitWindow: minExitWindow,
            bootstrapDeadline: block.timestamp + BOOTSTRAP_WINDOW
        });
    }

    function _verifierWithTiming(
        B3Types.SetHeader memory genesis,
        uint256 maxLag,
        uint256 maxCertificateAge,
        uint256 minExitWindow
    ) private returns (B3FinalityVerifier) {
        B3FinalityVerifier verifier = _uninitializedVerifierWithTiming(maxLag, maxCertificateAge, minExitWindow);
        verifier.initialize(_finalized(M - 1, bytes32(0), _setHash(genesis), 0), genesis, "");
        return verifier;
    }

    function _set(uint64 epoch, uint32 validators, uint64 totalWeight, uint8 seed)
        private
        pure
        returns (B3Types.SetHeader memory header)
    {
        header.epoch = epoch;
        header.rulesetVersion = 1;
        header.validatorCount = validators;
        header.totalWeight = totalWeight;
        header.quorumWeight = uint64((uint256(totalWeight) * 2) / 3 + 1);
        header.aggregatePubkey = _publicKey(seed);
        header.membersRoot = keccak256(abi.encodePacked("fixture-members", seed));
    }

    function _publicKey(uint8 seed) private pure returns (bytes memory key) {
        key = new bytes(48);
        key[0] = bytes1(uint8(0x80 | (seed & 0x1f)));
        key[47] = bytes1(seed);
    }

    function _recipient(uint160 keyHash) private pure returns (bytes32) {
        return bytes32((uint256(1) << 160) | uint256(keyHash));
    }

    function _setHash(B3Types.SetHeader memory header) private pure returns (bytes32) {
        return keccak256(
            abi.encodePacked(
                header.epoch,
                header.rulesetVersion,
                header.validatorCount,
                header.totalWeight,
                header.quorumWeight,
                header.aggregatePubkey,
                header.membersRoot
            )
        );
    }

    function _emptyWithdrawalRoot() private pure returns (bytes32 root) {
        for (uint256 level = 0; level < 32; ++level) {
            root = keccak256(abi.encodePacked(root, root));
        }
    }

    function _finalized(uint64 height, bytes32 withdrawalRoot, bytes32 successorHash, uint64 epoch)
        private
        pure
        returns (B3Types.FinalizedBlock memory)
    {
        return B3Types.FinalizedBlock({
            height: height,
            blockHash: keccak256(abi.encodePacked("fixture-block", height)),
            withdrawalRoot: withdrawalRoot,
            validatorSetHash: successorHash,
            epoch: epoch
        });
    }

    function _withdrawalRoot(B3StakerBridge bridge, B3StakerBridge.Withdrawal memory withdrawal)
        private
        view
        returns (bytes32 node, bytes32[32] memory path)
    {
        node = bridge.withdrawalLeaf(withdrawal);
        bytes32 zero;
        for (uint256 level = 0; level < path.length; ++level) {
            path[level] = zero;
            node = ((uint256(withdrawal.withdrawalId) >> level) & 1) == 0
                ? keccak256(abi.encodePacked(node, zero))
                : keccak256(abi.encodePacked(zero, node));
            zero = keccak256(abi.encodePacked(zero, zero));
        }
    }
}
