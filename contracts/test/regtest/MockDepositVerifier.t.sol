// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

// REGTEST ONLY: tests for contracts/regtest/MockDepositVerifier.sol against
// the production B3StakerBridge (regtest plan RT2/RT3a).

import {TestBase} from "../TestBase.sol";
import {B3BridgeAssetId} from "../../src/B3BridgeAssetId.sol";
import {B3FinalityVerifier} from "../../src/B3FinalityVerifier.sol";
import {B3StakerBridge} from "../../src/B3StakerBridge.sol";
import {MockDepositVerifier} from "../../regtest/MockDepositVerifier.sol";
import {MockTetherToken} from "../../regtest/MockTetherToken.sol";

contract MockDepositVerifierTest is TestBase {
    bytes32 private constant DOMAIN = keccak256("regtest-chain-domain");
    uint64 private constant ACTIVATION = 163;
    uint256 private constant MAX_DEPOSIT_RAW = 1e9;
    address private constant ALICE = address(0xA11CE);

    event Deposit(uint64 indexed depositId, address indexed token, uint256 amount, bytes32 b3Recipient);

    MockDepositVerifier private verifier;
    MockTetherToken private token;
    B3StakerBridge private bridge;

    function setUp() public {
        verifier = new MockDepositVerifier(DOMAIN, ACTIVATION);
        token = new MockTetherToken();
        bridge = _bridge(address(verifier), address(verifier).codehash, DOMAIN);
        token.issue(3 * MAX_DEPOSIT_RAW);
        token.approve(address(bridge), type(uint256).max);
    }

    function test_MockViews() public view {
        assertEq(verifier.CHAIN_DOMAIN(), DOMAIN);
        assertEq(verifier.BRIDGE_ACTIVATION_HEIGHT(), ACTIVATION);
        assertTrue(verifier.depositViable());
        assertFalse(verifier.releaseReady());
        assertEq(verifier.latestBridgeFinalizedHeight(), 0);
        assertEq(verifier.latestBridgeWithdrawalRoot(), bytes32(0));
    }

    function test_BridgeDeploysAgainstMockAndMirrorsPins() public view {
        assertEq(uint256(uint160(address(bridge.verifier()))), uint256(uint160(address(verifier))));
        assertEq(bridge.VERIFIER_CODE_HASH(), address(verifier).codehash);
        assertEq(bridge.B3_CHAIN_DOMAIN(), DOMAIN);
        assertEq(bridge.ORIGIN_CHAIN_ID(), block.chainid);
        assertEq(uint256(uint160(bridge.ORIGIN_TOKEN())), uint256(uint160(address(token))));
        assertEq(bridge.MAX_DEPOSIT_RAW(), MAX_DEPOSIT_RAW);
        assertEq(bridge.BRIDGE_ACTIVATION_HEIGHT(), ACTIVATION);
        assertEq(
            bridge.B3_ASSET_ID(),
            B3BridgeAssetId.compute(DOMAIN, uint64(block.chainid), address(bridge), address(token))
        );
    }

    function test_BridgeConstructorChecksMockCodeHashAndDomain() public {
        // Immutables are part of the runtime code, so the pinned code hash
        // depends on the mock's constructor arguments.
        MockDepositVerifier other = new MockDepositVerifier(DOMAIN, ACTIVATION + 1);
        assertTrue(address(other).codehash != address(verifier).codehash);

        vm.expectRevert(B3StakerBridge.BadBootstrap.selector);
        _bridge(address(verifier), address(other).codehash, DOMAIN);
        vm.expectRevert(B3StakerBridge.BadBootstrap.selector);
        _bridge(address(verifier), address(verifier).codehash, keccak256("other-domain"));

        MockDepositVerifier zeroDomain = new MockDepositVerifier(bytes32(0), ACTIVATION);
        vm.expectRevert(B3StakerBridge.BadBootstrap.selector);
        _bridge(address(zeroDomain), address(zeroDomain).codehash, bytes32(0));
    }

    function test_DepositEmitsEventAndLocksReceivedAmount() public {
        vm.expectEmit(true, true, false, true, address(bridge));
        emit Deposit(0, address(token), 25_000_000, _recipient(0xB3));
        bridge.deposit(25_000_000, _recipient(0xB3));

        vm.expectEmit(true, true, false, true, address(bridge));
        emit Deposit(1, address(token), 1, _recipient(0xB4));
        bridge.deposit(1, _recipient(0xB4));

        assertEq(bridge.nextDepositId(), 2);
        assertEq(bridge.locked(), 25_000_001);
        assertEq(token.balanceOf(address(bridge)), 25_000_001);
        assertEq(token.balanceOf(address(this)), 3 * MAX_DEPOSIT_RAW - 25_000_001);
    }

    function test_DepositClosedWhenMockNotViable() public {
        verifier.setDepositViable(false);
        vm.expectRevert(B3StakerBridge.DepositClosed.selector);
        bridge.deposit(1, _recipient(0xB3));
        assertEq(bridge.locked(), 0);

        verifier.setDepositViable(true);
        bridge.deposit(1, _recipient(0xB3));
        assertEq(bridge.locked(), 1);
    }

    function test_DepositTransferFailedWhenTokenPaused() public {
        token.pause();
        vm.expectRevert(B3StakerBridge.TransferFailed.selector);
        bridge.deposit(1, _recipient(0xB3));
        assertEq(bridge.locked(), 0);
        assertEq(bridge.nextDepositId(), 0);

        token.unpause();
        bridge.deposit(1, _recipient(0xB3));
        assertEq(bridge.locked(), 1);
    }

    function test_DepositTransferFailedWhenSenderBlackListed() public {
        token.transfer(ALICE, 10);
        vm.prank(ALICE);
        token.approve(address(bridge), 10);
        token.addBlackList(ALICE);

        vm.prank(ALICE);
        vm.expectRevert(B3StakerBridge.TransferFailed.selector);
        bridge.deposit(10, _recipient(0xB3));
        assertEq(token.balanceOf(ALICE), 10);
        assertEq(bridge.locked(), 0);

        token.removeBlackList(ALICE);
        vm.prank(ALICE);
        bridge.deposit(10, _recipient(0xB3));
        assertEq(bridge.locked(), 10);
    }

    function test_DepositCapExceeded() public {
        bridge.deposit(MAX_DEPOSIT_RAW, _recipient(0xB3));
        vm.expectRevert(B3StakerBridge.DepositCapExceeded.selector);
        bridge.deposit(MAX_DEPOSIT_RAW + 1, _recipient(0xB3));
        assertEq(bridge.locked(), MAX_DEPOSIT_RAW);
        assertEq(bridge.nextDepositId(), 1);
        assertEq(token.balanceOf(address(bridge)), MAX_DEPOSIT_RAW);
    }

    function test_DepositRejectsBadB3Recipient() public {
        vm.expectRevert(B3StakerBridge.BadB3Recipient.selector);
        bridge.deposit(1, bytes32(uint256(0xB3))); // missing version byte
        vm.expectRevert(B3StakerBridge.BadB3Recipient.selector);
        bridge.deposit(1, bytes32(type(uint256).max)); // nonzero padding
        vm.expectRevert(B3StakerBridge.BadB3Recipient.selector);
        bridge.deposit(1, _recipient(0)); // zero key hash
        assertEq(bridge.locked(), 0);
        assertEq(token.balanceOf(address(bridge)), 0);
    }

    function test_ReleaseNeverReady() public {
        bridge.deposit(5, _recipient(0xB3));
        bytes32[32] memory path;
        vm.expectRevert(B3StakerBridge.BridgeNotReady.selector);
        bridge.release(B3StakerBridge.Withdrawal(0, ALICE, 5, ACTIVATION), path);
        assertEq(bridge.locked(), 5);
    }

    function _bridge(address verifier_, bytes32 codeHash, bytes32 domain) private returns (B3StakerBridge) {
        return new B3StakerBridge(B3FinalityVerifier(verifier_), codeHash, domain, address(token), MAX_DEPOSIT_RAW);
    }

    function _recipient(uint160 keyHash) private pure returns (bytes32) {
        return bytes32((uint256(1) << 160) | uint256(keyHash));
    }
}
