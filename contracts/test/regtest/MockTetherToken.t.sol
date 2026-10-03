// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

// REGTEST ONLY: tests for contracts/regtest/MockTetherToken.sol.

import {TestBase} from "../TestBase.sol";
import {B3FinalityVerifier} from "../../src/B3FinalityVerifier.sol";
import {B3StakerBridge} from "../../src/B3StakerBridge.sol";
import {MockDepositVerifier} from "../../regtest/MockDepositVerifier.sol";
import {MockTetherToken} from "../../regtest/MockTetherToken.sol";

contract MockTetherTokenTest is TestBase {
    address private constant ALICE = address(0xA11CE);
    address private constant BOB = address(0xB0B);
    address private constant UPGRADED = address(0x0BADC0DE);
    uint256 private constant ADDRESS_MASK = type(uint160).max;

    MockTetherToken private token;

    function setUp() public {
        token = new MockTetherToken();
    }

    function test_Slot0PacksOwnerAndPausedByte() public {
        uint256 word = _slot(0);
        assertEq(word & ADDRESS_MASK, uint256(uint160(address(this))));
        assertEq(word >> 160, 0);
        assertFalse(token.paused());

        token.pause();
        word = _slot(0);
        assertTrue(token.paused());
        assertEq(word & ADDRESS_MASK, uint256(uint160(address(this))));
        assertEq(word >> 160, 1); // byte 20 is exactly 0x01

        token.unpause();
        assertEq(_slot(0) >> 160, 0);
    }

    function test_SupplyBalancesAndAllowedUseTetherSlots() public {
        token.issue(1_000);
        token.transfer(ALICE, 250);
        token.approve(BOB, 77);

        assertEq(_slot(1), 1_000);
        assertEq(token.totalSupply(), 1_000);
        assertEq(_balanceSlot(address(this)), 750);
        assertEq(_balanceSlot(ALICE), 250);
        assertEq(token.balanceOf(ALICE), 250);
        assertEq(_slot(3), 0);
        assertEq(_slot(4), 0);
        assertEq(token.basisPointsRate(), 0);
        assertEq(token.maximumFee(), 0);

        bytes32 inner = keccak256(abi.encode(address(this), uint256(5)));
        assertEq(uint256(vm.load(address(token), keccak256(abi.encode(BOB, inner)))), 77);
        assertEq(token.allowance(address(this), BOB), 77);
    }

    function test_BlackListUsesSlot6() public {
        bytes32 key = keccak256(abi.encode(ALICE, uint256(6)));
        assertEq(uint256(vm.load(address(token), key)), 0);
        token.addBlackList(ALICE);
        assertEq(uint256(vm.load(address(token), key)), 1);
        assertTrue(token.isBlackListed(ALICE));
        token.removeBlackList(ALICE);
        assertEq(uint256(vm.load(address(token), key)), 0);
    }

    function test_MetadataUsesSlots7To9() public view {
        // Short strings are stored inline: bytes left-aligned, length * 2 in
        // the lowest byte.
        bytes32 nameWord = "Mock Tether USD";
        bytes32 symbolWord = "USDT";
        assertEq(vm.load(address(token), bytes32(uint256(7))), nameWord | bytes32(uint256(15 * 2)));
        assertEq(vm.load(address(token), bytes32(uint256(8))), symbolWord | bytes32(uint256(4 * 2)));
        assertEq(_slot(9), 6);
        assertEq(token.decimals(), 6);
    }

    function test_Slot10PacksUpgradedAddressAndDeprecatedByte() public {
        assertEq(_slot(10), 0);
        token.deprecate(UPGRADED);
        uint256 word = _slot(10);
        assertEq(word & ADDRESS_MASK, uint256(uint160(UPGRADED)));
        assertEq(word >> 160, 1); // byte 20 is exactly 0x01
        assertTrue(token.deprecated());

        // Forwarding is deferred: the mock keeps serving its own balances.
        token.issue(5);
        token.transfer(ALICE, 5);
        assertEq(token.balanceOf(ALICE), 5);
    }

    function test_TransferApproveAndTransferFromReturnNoData() public {
        token.issue(100);
        bytes memory ret;
        bool ok;

        (ok, ret) = address(token).call(abi.encodeWithSignature("transfer(address,uint256)", ALICE, 10));
        assertTrue(ok);
        assertEq(ret.length, 0);

        (ok, ret) = address(token).call(abi.encodeWithSignature("approve(address,uint256)", BOB, 30));
        assertTrue(ok);
        assertEq(ret.length, 0);

        vm.prank(BOB);
        (ok, ret) = address(token)
            .call(abi.encodeWithSignature("transferFrom(address,address,uint256)", address(this), BOB, 20));
        assertTrue(ok);
        assertEq(ret.length, 0);
        assertEq(token.balanceOf(BOB), 20);
        assertEq(token.allowance(address(this), BOB), 10);
    }

    function test_PauseStopsTransfersButNotApprove() public {
        token.issue(100);
        token.approve(BOB, 50);
        token.pause();

        assertFalse(_transfer(address(this), ALICE, 1));
        assertFalse(_transferFrom(BOB, address(this), BOB, 1));
        token.approve(BOB, 0); // approve is not pausable on Tether

        token.unpause();
        assertTrue(_transfer(address(this), ALICE, 1));
        assertEq(token.balanceOf(ALICE), 1);
    }

    function test_BlackListRefusesSenderAndFromOnly() public {
        token.issue(100);
        token.transfer(ALICE, 40);
        vm.prank(ALICE);
        token.approve(BOB, type(uint256).max);
        token.approve(ALICE, 30);
        token.addBlackList(ALICE);

        assertFalse(_transfer(ALICE, BOB, 1)); // blacklisted sender
        assertFalse(_transferFrom(BOB, ALICE, BOB, 1)); // blacklisted from
        assertTrue(_transfer(address(this), ALICE, 1)); // receiving is allowed
        assertTrue(_transferFrom(ALICE, address(this), BOB, 2)); // blacklisted spender only
        assertEq(token.balanceOf(BOB), 2);

        token.removeBlackList(ALICE);
        assertTrue(_transfer(ALICE, BOB, 1));
    }

    function test_AllowanceRulesFollowTether() public {
        token.issue(100);
        token.approve(BOB, 5);
        (bool ok,) = address(token).call(abi.encodeWithSignature("approve(address,uint256)", BOB, 6));
        assertFalse(ok); // non-zero to non-zero is refused
        token.approve(BOB, 0);
        token.approve(BOB, 6);

        assertFalse(_transferFrom(BOB, address(this), BOB, 7)); // above allowance
        assertTrue(_transferFrom(BOB, address(this), BOB, 6));
        assertEq(token.allowance(address(this), BOB), 0);

        token.approve(ALICE, type(uint256).max);
        assertTrue(_transferFrom(ALICE, address(this), ALICE, 10));
        assertEq(token.allowance(address(this), ALICE), type(uint256).max);
    }

    function test_OwnerOnlyControls() public {
        bytes[5] memory calls = [
            abi.encodeWithSignature("issue(uint256)", 1),
            abi.encodeWithSignature("addBlackList(address)", BOB),
            abi.encodeWithSignature("removeBlackList(address)", BOB),
            abi.encodeWithSignature("deprecate(address)", UPGRADED),
            abi.encodeWithSignature("pause()")
        ];
        for (uint256 i = 0; i < calls.length; ++i) {
            vm.prank(ALICE);
            (bool ok,) = address(token).call(calls[i]);
            assertFalse(ok);
        }
        token.pause();
        vm.prank(ALICE);
        (bool unpaused,) = address(token).call(abi.encodeWithSignature("unpause()"));
        assertFalse(unpaused);
        assertTrue(token.paused());

        (bool zeroIssue,) = address(token).call(abi.encodeWithSignature("issue(uint256)", 0));
        assertFalse(zeroIssue);
        assertTrue(token.owner() == address(this));
        assertEq(token.totalSupply(), 0);
        assertFalse(token.isBlackListed(BOB));
        assertFalse(token.deprecated());
    }

    function test_NoReturnTransferFromWorksThroughB3StakerBridge() public {
        bytes32 domain = keccak256("regtest-chain-domain");
        MockDepositVerifier verifier = new MockDepositVerifier(domain, 163);
        B3StakerBridge bridge = new B3StakerBridge(
            B3FinalityVerifier(address(verifier)), address(verifier).codehash, domain, address(token), 1e9
        );
        token.issue(1_000_000);
        token.approve(address(bridge), 1_000_000);

        bridge.deposit(25_000, bytes32((uint256(1) << 160) | uint256(0xB3)));
        assertEq(bridge.locked(), 25_000);
        assertEq(token.balanceOf(address(bridge)), 25_000);
        assertEq(token.allowance(address(this), address(bridge)), 975_000);
        // The vault reserve is readable at the slot B3 proves on mainnet.
        assertEq(_balanceSlot(address(bridge)), bridge.locked());
    }

    function _slot(uint256 slot) private view returns (uint256) {
        return uint256(vm.load(address(token), bytes32(slot)));
    }

    function _balanceSlot(address account) private view returns (uint256) {
        return uint256(vm.load(address(token), keccak256(abi.encode(account, uint256(2)))));
    }

    function _transfer(address from, address to, uint256 value) private returns (bool ok) {
        vm.prank(from);
        (ok,) = address(token).call(abi.encodeWithSignature("transfer(address,uint256)", to, value));
    }

    function _transferFrom(address spender, address from, address to, uint256 value) private returns (bool ok) {
        vm.prank(spender);
        (ok,) = address(token).call(abi.encodeWithSignature("transferFrom(address,address,uint256)", from, to, value));
    }
}
