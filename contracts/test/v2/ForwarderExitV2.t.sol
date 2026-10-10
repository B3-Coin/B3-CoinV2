// SPDX-License-Identifier: MIT
pragma solidity ^0.8.28;

import {ConsoleV2} from "./V2TestBase.sol";
import {ForwarderFixtureV2, LzPacketsV2} from "./utils/ForwarderFixtureV2.sol";
import {MerkleTreeV2} from "./utils/MerkleTreeV2.sol";
import {RuntimeScanV2 as Scan} from "./utils/RuntimeScanV2.sol";
import {B3TypesV2} from "../../src/v2/B3TypesV2.sol";
import {B3Forwarder} from "../../src/v2/B3Forwarder.sol";
import {B3ForwarderTypesV2} from "../../src/v2/B3ForwarderTypesV2.sol";
import {B3OftExitFactory} from "../../src/v2/B3OftExitFactory.sol";
import {IOFTV2} from "../../src/v2/IOFTV2.sol";
import {MockLzEndpoint} from "../../regtest/MockLzEndpoint.sol";
import {MockOFTBase} from "../../regtest/MockOFTBase.sol";

/// B3OftExitFactory and route (d3) (contracts spec §8, §10, §12; regtest plan
/// RT6): Vault V2 release -> F_x -> USDT0 OFT adapter -> Arbitrum. Pins and
/// address derivation; a forward sends the balance less the fee through the
/// adapter with the exact SendParam, pays the fee, resets the allowance and
/// returns unused ETH; the fee cap per forward; each forward empties the
/// account, so it runs once per arrival and a reused F_x pays at most feeCap
/// per withdrawal; a short native fee or a paused endpoint reverts and the
/// funds wait for withdrawOnEthereum; isValid; slippage against the bridge
/// fee; Published; the runtime rules.
contract ForwarderExitV2Test is ForwarderFixtureV2 {
    using LzPacketsV2 for LzPacketsV2.Packet;
    using MerkleTreeV2 for bytes32[];

    uint256 private constant RELAYER_ETH = 1 ether;

    function setUp() public override {
        super.setUp();
        vm.deal(RELAYER, RELAYER_ETH);
    }

    // ============================================================= config

    function test_ExitFactoryPinsTokenAdapterAndDestination() public view {
        assertEq(fx.TOKEN(), address(usdt));
        assertEq(fx.ADAPTER(), address(adapter));
        assertEq(uint256(fx.DST_EID()), ARB_EID);
        assertEq(fx.extraOptions(), EXTRA_OPTIONS);
        assertEq(fx.FORWARDER_INIT_CODE_HASH(), keccak256(type(B3Forwarder).creationCode));
    }

    function test_ExitFactoryConstructorRefusesBadConfig() public {
        vm.expectRevert(B3OftExitFactory.BadConfig.selector);
        new B3OftExitFactory(address(0xC0DE1E55), address(adapter), ARB_EID, "");
        vm.expectRevert(B3OftExitFactory.BadConfig.selector);
        new B3OftExitFactory(address(usdt), address(0xC0DE1E55), ARB_EID, "");
        // The Arbitrum OFT's token() is itself, not Ethereum's USDT.
        vm.expectRevert(B3OftExitFactory.BadConfig.selector);
        new B3OftExitFactory(address(usdt), address(usdt0), ARB_EID, "");
        // Code without token().
        vm.expectRevert(B3OftExitFactory.BadConfig.selector);
        new B3OftExitFactory(address(usdt), address(usdt), ARB_EID, "");
        vm.expectRevert(B3OftExitFactory.BadConfig.selector);
        new B3OftExitFactory(address(usdt), address(adapter), 0, "");
    }

    /// accountOf is the CREATE2 address of B3Forwarder for the 192-byte exit
    /// salt preimage; the first forward deploys the account there.
    function test_AccountOfIsCreate2AndTheDeployment() public {
        B3ForwarderTypesV2.ExitParams memory p = exitParams();
        bytes memory preimage = B3ForwarderTypesV2.exitSaltPreimage(p);
        assertEq(preimage.length, 192);
        assertEq(
            preimage,
            abi.encode(
                keccak256("B3/FWD/EXIT/OFT/V2"),
                uint256(p.dstEid),
                p.to,
                uint256(p.maxBridgeFeeBps),
                uint256(p.feeCap),
                p.refundEOA
            )
        );
        bytes32 salt = keccak256(preimage);
        assertEq(B3ForwarderTypesV2.exitSalt(p), salt);
        address expected = address(
            uint160(
                uint256(
                    keccak256(
                        abi.encodePacked(bytes1(0xff), address(fx), salt, keccak256(type(B3Forwarder).creationCode))
                    )
                )
            )
        );
        assertEq(fx.accountOf(p), expected);
        assertEq(B3ForwarderTypesV2.create2Address(address(fx), salt, fx.FORWARDER_INIT_CODE_HASH()), expected);

        // Every field moves the address.
        B3ForwarderTypesV2.ExitParams memory q = exitParams();
        q.dstEid = ETH_EID;
        assertTrue(fx.accountOf(q) != expected);
        q = exitParams();
        q.to = word(CAROL);
        assertTrue(fx.accountOf(q) != expected);
        q = exitParams();
        q.maxBridgeFeeBps = 49;
        assertTrue(fx.accountOf(q) != expected);
        q = exitParams();
        q.feeCap += 1;
        assertTrue(fx.accountOf(q) != expected);
        q = exitParams();
        q.refundEOA = BOB;
        assertTrue(fx.accountOf(q) != expected);

        assertEq(expected.code.length, 0);
        fundUsdt(expected, 10 * UNIT);
        _forward(p, FEE, RELAYER, RELAYER, _quote(p, 10 * UNIT - FEE));
        assertGt(expected.code.length, 0);
        assertEq(B3Forwarder(expected).FACTORY(), address(fx));
    }

    // ============================================================ forward

    /// The balance less the fee goes through the adapter to p.to on Arbitrum,
    /// with SendParam(30110, p.to, amount, amount less 0.5%, EXTRA_OPTIONS,
    /// "", ""), msg.value exactly the quote and the change refunded.
    function test_ForwardSendsBalanceLessFeeThroughTheAdapter() public {
        B3ForwarderTypesV2.ExitParams memory p = exitParams();
        address account = fx.accountOf(p);
        uint256 balance = 1_000 * UNIT;
        uint256 sent = balance - FEE;
        fundUsdt(account, balance); // what the vault's release pays (see the route (d3) test)

        uint256 quote = fx.quoteForward(p, sent);
        assertEq(quote, endpointEth.quote());
        IOFTV2.SendParam memory sp = IOFTV2.SendParam({
            dstEid: ARB_EID,
            to: word(BOB),
            amountLD: sent,
            minAmountLD: sent - (sent * 50) / 10_000,
            extraOptions: EXTRA_OPTIONS,
            composeMsg: "",
            oftCmd: ""
        });
        CHEATS.expectCall(
            address(adapter), quote, abi.encodeCall(IOFTV2.send, (sp, IOFTV2.MessagingFee(quote, 0), RELAYER))
        );
        vm.recordLogs();
        vm.prank(RELAYER, RELAYER);
        assertEq(fx.forward{value: quote + 0.01 ether}(p, FEE, RELAYER2, RELAYER), sent);
        LzPacketsV2.Packet memory pk = LzPacketsV2.only(vm.getRecordedLogs(), address(endpointEth));

        assertEq(uint256(pk.dstEid), ARB_EID);
        assertEq(pk.sender, address(adapter));
        assertEq(pk.receiver, word(address(usdt0)));
        assertEq(pk.to(), p.to);
        assertEq(pk.amount(), sent);

        assertEq(usdt.balanceOf(RELAYER2), FEE, "fee to feeRecipient");
        assertEq(usdt.balanceOf(account), 0, "emptied");
        assertEq(usdt.allowance(account, address(adapter)), 0, "allowance reset");
        assertEq(adapter.lockedBalance(), sent, "locked by the adapter");
        assertEq(RELAYER.balance, RELAYER_ETH - quote, "change refunded");
        assertEq(account.balance, 0);
        assertEq(address(fx).balance, 0);
        assertEq(address(endpointEth).balance, quote);

        pk.deliver(endpointArb);
        assertEq(usdt0.balanceOf(BOB), sent, "arrived on Arbitrum");
    }

    function test_ForwardEmitsForwarded() public {
        B3ForwarderTypesV2.ExitParams memory p = exitParams();
        address account = fx.accountOf(p);
        fundUsdt(account, 100 * UNIT);
        uint256 quote = _quote(p, 100 * UNIT - FEE);
        bytes32 guid = endpointEth.guidOf(1, ETH_EID, word(address(adapter)), ARB_EID, word(address(usdt0)));

        vm.expectEmit(true, true, true, true, address(fx));
        emit B3OftExitFactory.Forwarded(account, p.to, guid, 100 * UNIT - FEE, 100 * UNIT - FEE, FEE, RELAYER, quote);
        _forward(p, FEE, RELAYER, RELAYER, quote);
    }

    function test_ForwardFeeAboveFeeCapReverts() public {
        B3ForwarderTypesV2.ExitParams memory p = exitParams();
        address account = fx.accountOf(p);
        fundUsdt(account, 100 * UNIT);
        uint256 quote = _quote(p, 100 * UNIT - EXIT_FEE_CAP);

        vm.expectRevert(B3OftExitFactory.FeeCapExceeded.selector);
        vm.prank(RELAYER, RELAYER);
        fx.forward{value: quote}(p, uint256(EXIT_FEE_CAP) + 1, RELAYER, RELAYER);
        assertEq(usdt.balanceOf(account), 100 * UNIT);

        assertEq(_forward(p, EXIT_FEE_CAP, RELAYER, RELAYER, quote), 100 * UNIT - EXIT_FEE_CAP);
        assertEq(usdt.balanceOf(RELAYER), EXIT_FEE_CAP);
    }

    /// Exactly once: a forward empties the account, so a second one has
    /// nothing to send.
    function test_ForwardExactlyOnce() public {
        B3ForwarderTypesV2.ExitParams memory p = exitParams();
        address account = fx.accountOf(p);
        fundUsdt(account, 100 * UNIT);
        uint256 quote = _quote(p, 100 * UNIT - FEE);
        _forward(p, FEE, RELAYER, RELAYER, quote);
        assertEq(endpointEth.outboundNonce(), 1);

        vm.expectRevert(B3OftExitFactory.NothingToForward.selector);
        vm.prank(RELAYER, RELAYER);
        fx.forward{value: quote}(p, FEE, RELAYER, RELAYER);
        vm.expectRevert(B3OftExitFactory.NothingToForward.selector);
        vm.prank(RELAYER, RELAYER);
        fx.forward{value: quote}(p, 0, RELAYER, RELAYER);

        // A balance of exactly the fee is nothing to forward either.
        fundUsdt(account, FEE);
        vm.expectRevert(B3OftExitFactory.NothingToForward.selector);
        vm.prank(RELAYER, RELAYER);
        fx.forward{value: quote}(p, FEE, RELAYER, RELAYER);
        assertEq(endpointEth.outboundNonce(), 1, "one packet");
    }

    /// One F_x address serves repeated withdrawals with the same parameters;
    /// each forward empties it and pays at most feeCap.
    function test_ReusedForwarderPaysAtMostFeeCapPerWithdrawal() public {
        B3ForwarderTypesV2.ExitParams memory p = exitParams();
        address account = fx.accountOf(p);

        fundUsdt(account, 300 * UNIT);
        uint256 quote = _quote(p, 300 * UNIT - EXIT_FEE_CAP);
        vm.recordLogs();
        _forward(p, EXIT_FEE_CAP, RELAYER, RELAYER, quote);
        LzPacketsV2.Packet memory first = LzPacketsV2.only(vm.getRecordedLogs(), address(endpointEth));
        assertEq(usdt.balanceOf(account), 0);

        fundUsdt(account, 200 * UNIT);
        vm.expectRevert(B3OftExitFactory.FeeCapExceeded.selector);
        vm.prank(RELAYER, RELAYER);
        fx.forward{value: quote}(p, uint256(EXIT_FEE_CAP) + 1, RELAYER, RELAYER);
        vm.recordLogs();
        _forward(p, EXIT_FEE_CAP, RELAYER, RELAYER, quote);
        LzPacketsV2.Packet memory second = LzPacketsV2.only(vm.getRecordedLogs(), address(endpointEth));
        assertEq(usdt.balanceOf(account), 0);

        assertEq(usdt.balanceOf(RELAYER), 2 * uint256(EXIT_FEE_CAP), "feeCap per withdrawal");
        first.deliver(endpointArb);
        second.deliver(endpointArb);
        assertEq(usdt0.balanceOf(BOB), 500 * UNIT - 2 * uint256(EXIT_FEE_CAP));
    }

    function test_NativeFeeShortReverts() public {
        B3ForwarderTypesV2.ExitParams memory p = exitParams();
        address account = fx.accountOf(p);
        fundUsdt(account, 100 * UNIT);
        uint256 quote = _quote(p, 100 * UNIT - FEE);

        vm.expectRevert(abi.encodeWithSelector(B3OftExitFactory.NativeFeeShort.selector, quote, quote - 1));
        vm.prank(RELAYER, RELAYER);
        fx.forward{value: quote - 1}(p, FEE, RELAYER, RELAYER);
        vm.expectRevert(abi.encodeWithSelector(B3OftExitFactory.NativeFeeShort.selector, quote, 0));
        vm.prank(RELAYER, RELAYER);
        fx.forward(p, FEE, RELAYER, RELAYER);

        // A fee rise at the endpoint has the same effect on an old quote.
        endpointEth.setNativeFee(quote * 2);
        vm.expectRevert(abi.encodeWithSelector(B3OftExitFactory.NativeFeeShort.selector, quote * 2, quote));
        vm.prank(RELAYER, RELAYER);
        fx.forward{value: quote}(p, FEE, RELAYER, RELAYER);
        assertEq(usdt.balanceOf(account), 100 * UNIT, "funds wait");
        assertEq(account.code.length, 0);
    }

    /// A LayerZero pause makes forward revert; the funds wait at F_x and its
    /// refundEOA takes them on Ethereum.
    function test_PausedEndpointFundsWaitAndWithdrawOnEthereum() public {
        B3ForwarderTypesV2.ExitParams memory p = exitParams();
        address account = fx.accountOf(p);
        fundUsdt(account, 100 * UNIT);
        uint256 quote = _quote(p, 100 * UNIT - FEE);
        endpointEth.setPaused(true);

        vm.expectRevert(MockLzEndpoint.EndpointPaused.selector);
        vm.prank(RELAYER, RELAYER);
        fx.forward{value: quote}(p, FEE, RELAYER, RELAYER);
        assertEq(usdt.balanceOf(account), 100 * UNIT, "funds wait");

        vm.expectRevert(B3OftExitFactory.NotRefundEOA.selector);
        vm.prank(BOB);
        fx.withdrawOnEthereum(p, address(usdt), BOB, 100 * UNIT);

        vm.expectEmit(true, true, true, true, address(fx));
        emit B3OftExitFactory.WithdrawnOnEthereum(account, address(usdt), CAROL, 100 * UNIT);
        vm.prank(ALICE);
        fx.withdrawOnEthereum(p, address(usdt), CAROL, 100 * UNIT);
        assertEq(usdt.balanceOf(CAROL), 100 * UNIT);
        assertTrue(account.code.length > 0, "deployed on demand");
    }

    function test_WithdrawOnEthereumTargetsAndTokens() public {
        B3ForwarderTypesV2.ExitParams memory p = exitParams();
        address account = fx.accountOf(p);
        fundUsdt(account, 10 * UNIT);
        vm.deal(account, 1 ether);

        address[3] memory targets = [address(0), account, address(fx)];
        for (uint256 i = 0; i < targets.length; ++i) {
            vm.expectRevert(B3OftExitFactory.BadWithdrawTarget.selector);
            vm.prank(ALICE);
            fx.withdrawOnEthereum(p, address(usdt), targets[i], UNIT);
        }
        vm.expectRevert(B3OftExitFactory.ZeroAmount.selector);
        vm.prank(ALICE);
        fx.withdrawOnEthereum(p, address(usdt), CAROL, 0);

        vm.expectRevert(B3OftExitFactory.InsufficientBalance.selector);
        vm.prank(ALICE);
        fx.withdrawOnEthereum(p, address(0), CAROL, 1 ether + 1);
        vm.prank(ALICE);
        fx.withdrawOnEthereum(p, address(0), CAROL, 1 ether);
        assertEq(CAROL.balance, 1 ether);

        vm.expectRevert(abi.encodeWithSelector(B3OftExitFactory.NoCode.selector, address(0xC0DE1E55)));
        vm.prank(ALICE);
        fx.withdrawOnEthereum(p, address(0xC0DE1E55), CAROL, 1);

        vm.prank(ALICE);
        fx.withdrawOnEthereum(p, address(usdt), ALICE, 10 * UNIT);
        assertEq(usdt.balanceOf(ALICE), 10 * UNIT);
        assertEq(usdt.balanceOf(account), 0);
    }

    // ============================================================ isValid

    function test_IsValidTable() public {
        assertTrue(fx.isValid(exitParams()));
        B3ForwarderTypesV2.ExitParams memory q = exitParams();
        q.maxBridgeFeeBps = 0;
        assertTrue(fx.isValid(q));
        q.maxBridgeFeeBps = 50;
        assertTrue(fx.isValid(q));

        B3ForwarderTypesV2.ExitParams[] memory bad = new B3ForwarderTypesV2.ExitParams[](7);
        for (uint256 i = 0; i < bad.length; ++i) {
            bad[i] = exitParams();
        }
        bad[0].dstEid = ETH_EID;
        bad[1].dstEid = 0;
        bad[2].to = bytes32((uint256(1) << 160) | uint256(uint160(BOB))); // upper bytes set
        bad[3].to = bytes32(0);
        bad[4].to = bytes32(type(uint256).max);
        bad[5].maxBridgeFeeBps = 51;
        bad[6].refundEOA = address(0);

        for (uint256 i = 0; i < bad.length; ++i) {
            assertFalse(fx.isValid(bad[i]));
            vm.expectRevert(B3OftExitFactory.InvalidParams.selector);
            fx.publish(bad[i]);
            fundUsdt(fx.accountOf(bad[i]), 10 * UNIT);
            vm.expectRevert(B3OftExitFactory.InvalidParams.selector);
            vm.prank(RELAYER, RELAYER);
            fx.forward{value: 1 ether}(bad[i], 0, RELAYER, RELAYER);
        }

        // Funds at an invalid p's account still leave by its refundEOA.
        address stuck = fx.accountOf(bad[5]);
        vm.prank(ALICE);
        fx.withdrawOnEthereum(bad[5], address(usdt), ALICE, 10 * UNIT);
        assertEq(usdt.balanceOf(stuck), 0);
    }

    /// maxBridgeFeeBps bounds the OFT's own fee: within it the send goes
    /// through and the recipient gets the amount less the bridge fee; above
    /// it the quote and the forward revert and the funds wait.
    function test_SlippageBoundWithMockBridgeFee() public {
        adapter.setFeeBps(30);
        B3ForwarderTypesV2.ExitParams memory p = exitParams();
        address account = fx.accountOf(p);
        fundUsdt(account, 1_000 * UNIT);
        uint256 sent = 1_000 * UNIT - FEE;
        uint256 received = sent - (sent * 30) / 10_000;

        vm.recordLogs();
        _forward(p, FEE, RELAYER, RELAYER, _quote(p, sent));
        LzPacketsV2.Packet memory pk = LzPacketsV2.only(vm.getRecordedLogs(), address(endpointEth));
        assertEq(pk.amount(), received);
        pk.deliver(endpointArb);
        assertEq(usdt0.balanceOf(BOB), received);

        B3ForwarderTypesV2.ExitParams memory tight = exitParams();
        tight.maxBridgeFeeBps = 20;
        address tightAccount = fx.accountOf(tight);
        fundUsdt(tightAccount, 1_000 * UNIT);
        bytes memory slippage =
            abi.encodeWithSelector(MockOFTBase.SlippageExceeded.selector, received, sent - (sent * 20) / 10_000);
        vm.expectRevert(slippage);
        fx.quoteForward(tight, sent);
        uint256 nativeFee = endpointEth.quote();
        vm.expectRevert(slippage);
        vm.prank(RELAYER, RELAYER);
        fx.forward{value: nativeFee}(tight, FEE, RELAYER, RELAYER);
        assertEq(usdt.balanceOf(tightAccount), 1_000 * UNIT, "funds wait");
    }

    function test_PublishEmitsPublished() public {
        B3ForwarderTypesV2.ExitParams memory p = exitParams();
        address account = fx.accountOf(p);
        vm.expectEmit(true, true, true, true, address(fx));
        emit B3OftExitFactory.Published(account, p);
        vm.prank(BOB);
        assertEq(fx.publish(p), account);
        assertEq(account.code.length, 0, "publish deploys nothing");
    }

    function test_FeeRecipientAndNativeRefundChecks() public {
        B3ForwarderTypesV2.ExitParams memory p = exitParams();
        address account = fx.accountOf(p);
        fundUsdt(account, 100 * UNIT);
        uint256 quote = _quote(p, 100 * UNIT - FEE);

        address[3] memory bad = [address(0), account, address(fx)];
        for (uint256 i = 0; i < bad.length; ++i) {
            vm.expectRevert(B3OftExitFactory.BadFeeRecipient.selector);
            vm.prank(RELAYER, RELAYER);
            fx.forward{value: quote}(p, FEE, bad[i], RELAYER);
        }
        vm.expectRevert(B3OftExitFactory.ZeroNativeRefundTo.selector);
        vm.prank(RELAYER, RELAYER);
        fx.forward{value: quote}(p, FEE, RELAYER, address(0));
        // Change to an address that refuses ETH reverts the whole forward.
        vm.expectRevert(B3OftExitFactory.NativeRefundFailed.selector);
        vm.prank(RELAYER, RELAYER);
        fx.forward{value: quote + 1}(p, FEE, RELAYER, address(usdt));

        // Without a fee no recipient is needed; the whole balance is sent.
        assertEq(_forward(p, 0, address(0), RELAYER, _quote(p, 100 * UNIT)), 100 * UNIT);
    }

    // ========================================================= route (d3)

    /// The vault releases a certified withdrawal to F_x; the forward carries
    /// it to Arbitrum, less the fee.
    function test_RouteD3_ReleaseToForwarderThenForward() public {
        B3ForwarderTypesV2.ExitParams memory p = exitParams();
        address account = fx.accountOf(p);

        fundUsdt(ALICE, 1_000 * UNIT);
        vm.prank(ALICE);
        usdt.approve(address(vault), 1_000 * UNIT);
        vm.prank(ALICE, ALICE);
        vault.deposit(1_000 * UNIT, B3_RECIPIENT);

        B3TypesV2.WithdrawalV2 memory w =
            B3TypesV2.WithdrawalV2({withdrawalId: 0, recipient: account, amount: 400 * UNIT, b3Height: V});
        bytes32[] memory leaves = new bytes32[](1);
        leaves[0] = vault.withdrawalLeaf(w);
        verifier.acceptRoot(leaves.root(1, 32), V);
        vault.release(w, leaves.path32(1, 0));
        assertEq(usdt.balanceOf(account), 400 * UNIT);
        assertEq(vault.locked(), 600 * UNIT);

        vm.recordLogs();
        _forward(p, FEE, RELAYER, RELAYER, _quote(p, 400 * UNIT - FEE));
        LzPacketsV2.Packet memory pk = LzPacketsV2.only(vm.getRecordedLogs(), address(endpointEth));
        pk.deliver(endpointArb);
        assertEq(usdt0.balanceOf(BOB), 400 * UNIT - FEE);
        assertEq(usdt.balanceOf(RELAYER), FEE);
        assertEq(usdt.balanceOf(account), 0);
    }

    // ============================================================ runtime

    function test_RuntimeSizesWithinEip170() public view {
        assertSizes("B3OftExitFactory", address(fx).code.length, type(B3OftExitFactory).creationCode.length);
        ConsoleV2.log("B3OftExitFactory runtime bytes", address(fx).code.length);
    }

    /// Spec §12: no NUMBER; the F_x runtime writes no storage (its options
    /// are written by the constructor), only its transient lock.
    function test_RuntimesHaveNoNumberOpcode() public {
        bytes memory code = address(fx).code;
        assertFactoryRuntimeClean("B3OftExitFactory", code);
        (uint256[256] memory counts,) = factoryOpcodeCounts(code);
        assertEq(counts[Scan.SSTORE], 0, "no SSTORE");
        assertGt(counts[Scan.TSTORE], 0);

        B3ForwarderTypesV2.ExitParams memory p = exitParams();
        fundUsdt(fx.accountOf(p), 10 * UNIT);
        _forward(p, FEE, RELAYER, RELAYER, _quote(p, 10 * UNIT - FEE));
        assertForwarderRuntimeClean(fx.accountOf(p));
    }

    // ============================================================ helpers

    function _quote(B3ForwarderTypesV2.ExitParams memory p, uint256 amount) private view returns (uint256) {
        return fx.quoteForward(p, amount);
    }

    /// A forward by RELAYER carrying exactly `value` wei.
    function _forward(
        B3ForwarderTypesV2.ExitParams memory p,
        uint256 fee,
        address feeRecipient,
        address nativeRefundTo,
        uint256 value
    ) private returns (uint256) {
        vm.prank(RELAYER, RELAYER);
        return fx.forward{value: value}(p, fee, feeRecipient, nativeRefundTo);
    }
}
