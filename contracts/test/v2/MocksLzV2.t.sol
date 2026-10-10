// SPDX-License-Identifier: MIT
pragma solidity ^0.8.28;

import {ForwarderFixtureV2, LzPacketsV2} from "./utils/ForwarderFixtureV2.sol";
import {IOFTV2} from "../../src/v2/IOFTV2.sol";
import {MockLzEndpoint} from "../../regtest/MockLzEndpoint.sol";
import {MockOFTBase} from "../../regtest/MockOFTBase.sol";
import {MockUSDT0} from "../../regtest/MockUSDT0.sol";

/// The regtest LayerZero and USDT0 doubles (regtest plan §3.1, §4.1; RT6):
/// MockLzEndpoint delivers each packet once and keeps a failed delivery
/// retryable; pause; the OFT exact-fee rule; MockOFTAdapter locks USDT on
/// "Ethereum" and unlocks it on delivery; MockUSDT0 burns and mints on
/// "Arbitrum", and a delivery to a blocked address mints frozen tokens the
/// owner may destroy. Both chains run in this one EVM (ForwarderFixtureV2).
contract MocksLzV2Test is ForwarderFixtureV2 {
    using LzPacketsV2 for LzPacketsV2.Packet;

    function test_EndpointsPeersAndTokens() public view {
        assertEq(uint256(endpointEth.eid()), ETH_EID);
        assertEq(uint256(endpointArb.eid()), ARB_EID);
        assertEq(uint256(adapter.localEid()), ETH_EID);
        assertEq(uint256(usdt0.localEid()), ARB_EID);
        assertEq(adapter.peers(ARB_EID), word(address(usdt0)));
        assertEq(usdt0.peers(ETH_EID), word(address(adapter)));
        assertEq(adapter.token(), address(usdt));
        assertEq(usdt0.token(), address(usdt0));
        assertTrue(adapter.approvalRequired());
        assertFalse(usdt0.approvalRequired());
        assertEq(uint256(usdt0.decimals()), 6);
        assertEq(endpointEth.quote(), endpointEth.DEFAULT_NATIVE_FEE());
        assertEq(endpointArb.quote(), 1e14);
    }

    // ============================================================ delivery

    function test_DeliveryRunsExactlyOnce() public {
        fundUsdt(address(adapter), 50 * UNIT);
        LzPacketsV2.Packet memory pk = sendFromArbitrum(BOB, word(CAROL), 50 * UNIT);
        assertEq(uint256(pk.nonce), 1);
        assertEq(uint256(pk.srcEid), ARB_EID);
        assertEq(uint256(pk.dstEid), ETH_EID);
        assertEq(pk.sender, address(usdt0));
        assertEq(pk.receiver, word(address(adapter)));
        assertEq(pk.to(), word(CAROL));
        assertEq(pk.amount(), 50 * UNIT);
        assertFalse(endpointEth.isDelivered(ARB_EID, word(address(usdt0)), 1));

        vm.prank(RELAYER); // anyone delivers
        pk.deliver(endpointEth);
        assertTrue(endpointEth.isDelivered(ARB_EID, word(address(usdt0)), 1));
        assertEq(usdt.balanceOf(CAROL), 50 * UNIT);
        assertEq(adapter.lockedBalance(), 0);

        vm.expectRevert(
            abi.encodeWithSelector(MockLzEndpoint.AlreadyDelivered.selector, ARB_EID, word(address(usdt0)), uint64(1))
        );
        pk.deliver(endpointEth);
        assertEq(usdt.balanceOf(CAROL), 50 * UNIT, "credited once");
    }

    /// A delivery the adapter cannot pay (no locked backing) reverts and
    /// stays undelivered; after seeding it is retried and credits once.
    function test_FailedDeliveryIsRetryableAndCreditsOnce() public {
        LzPacketsV2.Packet memory pk = sendFromArbitrum(BOB, word(CAROL), 50 * UNIT);
        vm.expectRevert();
        pk.deliver(endpointEth);
        assertFalse(endpointEth.isDelivered(ARB_EID, word(address(usdt0)), pk.nonce));
        assertEq(usdt.balanceOf(CAROL), 0);

        fundUsdt(address(adapter), 50 * UNIT);
        pk.deliver(endpointEth);
        assertTrue(endpointEth.isDelivered(ARB_EID, word(address(usdt0)), pk.nonce));
        assertEq(usdt.balanceOf(CAROL), 50 * UNIT);

        vm.expectRevert(
            abi.encodeWithSelector(
                MockLzEndpoint.AlreadyDelivered.selector, ARB_EID, word(address(usdt0)), uint64(pk.nonce)
            )
        );
        pk.deliver(endpointEth);
        assertEq(usdt.balanceOf(CAROL), 50 * UNIT, "credited once");
        assertEq(adapter.lockedBalance(), 0);
    }

    function test_PauseStopsSendAndDelivery() public {
        vm.expectRevert(MockLzEndpoint.NotOwner.selector);
        vm.prank(BOB);
        endpointArb.setPaused(true);

        endpointArb.setPaused(true);
        usdt0.mint(BOB, 10 * UNIT);
        uint256 fee = endpointArb.quote();
        vm.deal(BOB, fee);
        vm.expectRevert(MockLzEndpoint.EndpointPaused.selector);
        vm.prank(BOB, BOB);
        usdt0.send{value: fee}(_sp(ETH_EID, word(CAROL), 10 * UNIT), IOFTV2.MessagingFee(fee, 0), BOB);
        assertEq(usdt0.balanceOf(BOB), 10 * UNIT, "nothing burned");
        endpointArb.setPaused(false);

        fundUsdt(address(adapter), 10 * UNIT);
        LzPacketsV2.Packet memory pk = sendFromArbitrum(BOB, word(CAROL), 10 * UNIT);
        endpointEth.setPaused(true);
        vm.expectRevert(MockLzEndpoint.EndpointPaused.selector);
        pk.deliver(endpointEth);
        assertFalse(endpointEth.isDelivered(ARB_EID, word(address(usdt0)), pk.nonce));
        endpointEth.setPaused(false);
        pk.deliver(endpointEth);
        assertEq(usdt.balanceOf(CAROL), 10 * UNIT);
    }

    // ================================================================= OFT

    /// Ethereum -> Arbitrum locks USDT in the adapter and mints USDT0;
    /// Arbitrum -> Ethereum burns USDT0 and unlocks USDT.
    function test_AdapterLocksAndUnlocks() public {
        fundUsdt(ALICE, 100 * UNIT);
        vm.prank(ALICE);
        usdt.approve(address(adapter), 100 * UNIT);
        uint256 fee = endpointEth.quote();
        vm.deal(ALICE, fee);

        vm.recordLogs();
        vm.prank(ALICE, ALICE);
        (IOFTV2.MessagingReceipt memory receipt, IOFTV2.OFTReceipt memory oft) =
            adapter.send{value: fee}(_sp(ARB_EID, word(BOB), 100 * UNIT), IOFTV2.MessagingFee(fee, 0), ALICE);
        LzPacketsV2.Packet memory out = LzPacketsV2.only(vm.getRecordedLogs(), address(endpointEth));
        assertEq(uint256(receipt.nonce), 1);
        assertEq(receipt.guid, endpointEth.guidOf(1, ETH_EID, word(address(adapter)), ARB_EID, word(address(usdt0))));
        assertEq(receipt.fee.nativeFee, fee);
        assertEq(oft.amountSentLD, 100 * UNIT);
        assertEq(oft.amountReceivedLD, 100 * UNIT);
        assertEq(adapter.lockedBalance(), 100 * UNIT, "locked");
        assertEq(usdt.balanceOf(ALICE), 0);
        assertEq(usdt.allowance(ALICE, address(adapter)), 0);

        out.deliver(endpointArb);
        assertEq(usdt0.balanceOf(BOB), 100 * UNIT, "minted");
        assertEq(usdt0.totalSupply(), 100 * UNIT);

        uint256 arbFee = endpointArb.quote();
        vm.deal(BOB, arbFee);
        vm.recordLogs();
        vm.prank(BOB, BOB);
        usdt0.send{value: arbFee}(_sp(ETH_EID, word(CAROL), 40 * UNIT), IOFTV2.MessagingFee(arbFee, 0), BOB);
        LzPacketsV2.Packet memory back = LzPacketsV2.only(vm.getRecordedLogs(), address(endpointArb));
        assertEq(usdt0.totalSupply(), 60 * UNIT, "burned");
        assertEq(usdt0.balanceOf(BOB), 60 * UNIT);

        back.deliver(endpointEth);
        assertEq(usdt.balanceOf(CAROL), 40 * UNIT, "unlocked");
        assertEq(adapter.lockedBalance(), 60 * UNIT, "backing of the USDT0 left on Arbitrum");
    }

    /// LayerZero's exact-fee rule: msg.value equals fee.nativeFee, which
    /// covers the endpoint's quote; any excess the endpoint returns to
    /// refundAddress. LZ-token fees are not modelled.
    function test_ExactNativeFeeRule() public {
        usdt0.mint(BOB, 10 * UNIT);
        vm.deal(BOB, 1 ether);
        uint256 fee = endpointArb.quote();
        IOFTV2.SendParam memory sp = _sp(ETH_EID, word(CAROL), 10 * UNIT);

        vm.expectRevert(abi.encodeWithSelector(MockOFTBase.NotEnoughNative.selector, fee + 1));
        vm.prank(BOB, BOB);
        usdt0.send{value: fee + 1}(sp, IOFTV2.MessagingFee(fee, 0), BOB);
        vm.expectRevert(abi.encodeWithSelector(MockOFTBase.NotEnoughNative.selector, fee - 1));
        vm.prank(BOB, BOB);
        usdt0.send{value: fee - 1}(sp, IOFTV2.MessagingFee(fee, 0), BOB);
        vm.expectRevert(abi.encodeWithSelector(MockOFTBase.NotEnoughNative.selector, fee - 1));
        vm.prank(BOB, BOB);
        usdt0.send{value: fee - 1}(sp, IOFTV2.MessagingFee(fee - 1, 0), BOB);
        vm.expectRevert(MockOFTBase.LzTokenUnavailable.selector);
        vm.prank(BOB, BOB);
        usdt0.send{value: fee}(sp, IOFTV2.MessagingFee(fee, 1), BOB);
        vm.expectRevert(MockOFTBase.LzTokenUnavailable.selector);
        usdt0.quoteSend(sp, true);

        vm.prank(BOB, BOB);
        usdt0.send{value: fee + 5}(sp, IOFTV2.MessagingFee(fee + 5, 0), CAROL);
        assertEq(CAROL.balance, 5, "excess to refundAddress");
        assertEq(address(endpointArb).balance, fee);
        assertEq(BOB.balance, 1 ether - fee - 5);
    }

    function test_SendRefusesComposeNoPeerSlippageAndOverflow() public {
        usdt0.mint(BOB, 10 * UNIT);
        vm.deal(BOB, 1 ether);
        uint256 fee = endpointArb.quote();

        IOFTV2.SendParam memory sp = _sp(ETH_EID, word(CAROL), 10 * UNIT);
        sp.composeMsg = hex"01";
        _expectSendRevert(sp, fee, abi.encodeWithSelector(MockOFTBase.ComposeUnsupported.selector));
        sp = _sp(ETH_EID, word(CAROL), 10 * UNIT);
        sp.oftCmd = hex"01";
        _expectSendRevert(sp, fee, abi.encodeWithSelector(MockOFTBase.ComposeUnsupported.selector));
        sp = _sp(999, word(CAROL), 10 * UNIT);
        _expectSendRevert(sp, fee, abi.encodeWithSelector(MockOFTBase.NoPeer.selector, uint32(999)));
        sp = _sp(ETH_EID, word(CAROL), uint256(type(uint64).max) + 1);
        _expectSendRevert(
            sp, fee, abi.encodeWithSelector(MockOFTBase.AmountSDOverflowed.selector, uint256(type(uint64).max) + 1)
        );

        usdt0.setFeeBps(10);
        sp = _sp(ETH_EID, word(CAROL), 10 * UNIT);
        uint256 received = 10 * UNIT - (10 * UNIT * 10) / 10_000;
        _expectSendRevert(sp, fee, abi.encodeWithSelector(MockOFTBase.SlippageExceeded.selector, received, 10 * UNIT));
        sp.minAmountLD = received;
        vm.prank(BOB, BOB);
        (, IOFTV2.OFTReceipt memory oft) = usdt0.send{value: fee}(sp, IOFTV2.MessagingFee(fee, 0), BOB);
        assertEq(oft.amountReceivedLD, received);
        assertEq(oft.amountSentLD, 10 * UNIT);
    }

    function test_LzReceiveOnlyFromEndpointAndPeer() public {
        bytes memory message = abi.encodePacked(word(CAROL), uint64(UNIT));
        vm.expectRevert(abi.encodeWithSelector(MockOFTBase.OnlyEndpoint.selector, address(this)));
        usdt0.lzReceive(ETH_EID, word(address(adapter)), 1, message);
        vm.expectRevert(abi.encodeWithSelector(MockOFTBase.OnlyPeer.selector, ETH_EID, word(BOB)));
        endpointArb.deliver(ETH_EID, word(BOB), 1, address(usdt0), message);
        vm.expectRevert(abi.encodeWithSelector(MockOFTBase.OnlyPeer.selector, uint32(999), word(address(adapter))));
        endpointArb.deliver(999, word(address(adapter)), 1, address(usdt0), message);
        vm.expectRevert(MockOFTBase.BadMessage.selector);
        endpointArb.deliver(ETH_EID, word(address(adapter)), 1, address(usdt0), hex"00");
        vm.expectRevert(abi.encodeWithSelector(MockLzEndpoint.NoCode.selector, CAROL));
        endpointArb.deliver(ETH_EID, word(address(adapter)), 1, CAROL, message);

        assertEq(usdt0.totalSupply(), 0);
        assertFalse(endpointArb.isDelivered(ETH_EID, word(address(adapter)), 1), "still deliverable");
        endpointArb.deliver(ETH_EID, word(address(adapter)), 1, address(usdt0), message);
        assertEq(usdt0.balanceOf(CAROL), UNIT);
    }

    function test_OwnerSetsFeesPeersAndPause() public {
        vm.expectRevert(MockLzEndpoint.NotOwner.selector);
        vm.prank(BOB);
        endpointEth.setNativeFee(1);
        endpointEth.setNativeFee(5e14);
        assertEq(endpointEth.quote(), 5e14);
        assertEq(adapter.quoteSend(_sp(ARB_EID, word(BOB), UNIT), false).nativeFee, 5e14);

        vm.expectRevert(MockOFTBase.NotOwner.selector);
        vm.prank(BOB);
        adapter.setPeer(1, word(BOB));
        vm.expectRevert(MockOFTBase.NotOwner.selector);
        vm.prank(BOB);
        adapter.setFeeBps(1);
        vm.expectRevert(MockOFTBase.BadFeeBps.selector);
        adapter.setFeeBps(10_001);
        vm.expectRevert(MockOFTBase.NotOwner.selector);
        vm.prank(BOB);
        usdt0.mint(BOB, 1);
    }

    // ============================================================ blocklist

    /// USDT0 checks only `from`: a delivery to a blocked address mints
    /// frozen tokens (contracts spec §8), which only the owner may destroy.
    function test_Usdt0BlockedRecipientReceivesFrozenTokens() public {
        usdt0.addBlocked(BOB);
        assertTrue(usdt0.isBlocked(BOB));

        fundUsdt(ALICE, 100 * UNIT);
        vm.prank(ALICE);
        usdt.approve(address(adapter), 100 * UNIT);
        uint256 fee = endpointEth.quote();
        vm.deal(ALICE, fee);
        vm.recordLogs();
        vm.prank(ALICE, ALICE);
        adapter.send{value: fee}(_sp(ARB_EID, word(BOB), 100 * UNIT), IOFTV2.MessagingFee(fee, 0), ALICE);
        LzPacketsV2.only(vm.getRecordedLogs(), address(endpointEth)).deliver(endpointArb);
        assertEq(usdt0.balanceOf(BOB), 100 * UNIT, "minted although blocked");

        vm.expectRevert(abi.encodeWithSelector(MockUSDT0.Blocked.selector, BOB));
        vm.prank(BOB);
        usdt0.transfer(CAROL, 1);
        vm.prank(BOB);
        usdt0.approve(CAROL, type(uint256).max);
        vm.expectRevert(abi.encodeWithSelector(MockUSDT0.Blocked.selector, BOB));
        vm.prank(CAROL);
        usdt0.transferFrom(BOB, CAROL, 1);
        uint256 arbFee = endpointArb.quote();
        vm.deal(BOB, arbFee);
        vm.expectRevert(abi.encodeWithSelector(MockUSDT0.Blocked.selector, BOB));
        vm.prank(BOB, BOB);
        usdt0.send{value: arbFee}(_sp(ETH_EID, word(BOB), UNIT), IOFTV2.MessagingFee(arbFee, 0), BOB);

        vm.expectRevert(MockOFTBase.NotOwner.selector);
        vm.prank(CAROL);
        usdt0.destroyBlockedFunds(BOB);
        vm.expectRevert(abi.encodeWithSelector(MockUSDT0.NotBlocked.selector, CAROL));
        usdt0.destroyBlockedFunds(CAROL);
        usdt0.destroyBlockedFunds(BOB);
        assertEq(usdt0.balanceOf(BOB), 0);
        assertEq(usdt0.totalSupply(), 0);
        assertEq(adapter.lockedBalance(), 100 * UNIT, "the backing stays locked on Ethereum");

        usdt0.removeBlocked(BOB);
        assertFalse(usdt0.isBlocked(BOB));
    }

    // ============================================================ helpers

    function _sp(uint32 dstEid, bytes32 to, uint256 amount) private pure returns (IOFTV2.SendParam memory) {
        return IOFTV2.SendParam({
            dstEid: dstEid, to: to, amountLD: amount, minAmountLD: amount, extraOptions: "", composeMsg: "", oftCmd: ""
        });
    }

    /// A USDT0 send by BOB that must revert with `reason`; so must its quote.
    function _expectSendRevert(IOFTV2.SendParam memory sp, uint256 fee, bytes memory reason) private {
        vm.expectRevert(reason);
        usdt0.quoteSend(sp, false);
        vm.expectRevert(reason);
        vm.prank(BOB, BOB);
        usdt0.send{value: fee}(sp, IOFTV2.MessagingFee(fee, 0), BOB);
    }
}
