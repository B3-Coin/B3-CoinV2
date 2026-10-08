// SPDX-License-Identifier: MIT
pragma solidity ^0.8.28;

import {VmV2, ConsoleV2} from "./V2TestBase.sol";
import {RelayFixtureV2} from "./utils/RelayFixtureV2.sol";
import {LzPacketsV2} from "./utils/ForwarderFixtureV2.sol";
import {BlsTestSignerV2 as Signer} from "./utils/BlsTestSignerV2.sol";
import {B3TypesV2} from "../../src/v2/B3TypesV2.sol";
import {B3FinalityVerifierV2} from "../../src/v2/B3FinalityVerifierV2.sol";
import {B3VaultV2} from "../../src/v2/B3VaultV2.sol";
import {B3RelayBatch} from "../../src/v2/B3RelayBatch.sol";
import {B3ForwarderTypesV2} from "../../src/v2/B3ForwarderTypesV2.sol";
import {B3OftExitFactory} from "../../src/v2/B3OftExitFactory.sol";
import {IB3FinalityVerifierV2} from "../../src/v2/IB3FinalityVerifierV2.sol";
import {MockLzEndpoint} from "../../regtest/MockLzEndpoint.sol";
import {MockOFTAdapter} from "../../regtest/MockOFTAdapter.sol";
import {MockUSDT0} from "../../regtest/MockUSDT0.sol";

/// TEST ONLY. A nativeRefundTo that, on its first ETH receipt, re-enters the
/// batch with an empty, value-free relayWithForwards naming `thief` as its
/// refund target. If the batch returned its whole balance instead of the
/// calling frame's own unspent budget, the inner call would hand the outer
/// call's ETH to `thief`.
contract ReentrantRefundeeV2 {
    B3RelayBatch private immutable batch;
    address private immutable thief;
    bool public reentered;
    uint256 public received;

    constructor(B3RelayBatch batch_, address thief_) {
        batch = batch_;
        thief = thief_;
    }

    receive() external payable {
        received += msg.value;
        if (reentered) return;
        reentered = true;
        batch.relayWithForwards(
            IB3FinalityVerifierV2(address(0)),
            new B3RelayBatch.CertInput[](0),
            0,
            address(0),
            new B3RelayBatch.ReleaseItem[](0),
            0,
            address(0),
            new B3RelayBatch.ExitForward[](0),
            0,
            thief
        );
    }
}

/// B3RelayBatch.relayWithForwards (contracts spec §8-§9, route (d3)) on the
/// real V2 stack plus the exit forwarder factory over the regtest LayerZero
/// mocks: a certificate, a release paying F_x and the forward of F_x to
/// Arbitrum in one call, with the F_x fee paying the relayer; the native-fee
/// budget (skips, the factory's change, the unspent rest returned, a
/// re-entrant call unable to take it); per-item isolation of forwards; a
/// repeated batch skipping the release and reporting NothingToForward; the
/// up-front refusals; and a 64-member transition certificate plus a release
/// plus a first forward within the EIP-7825 cap.
///
/// setUp: as RelayBatchV2Test (K_OF_N verifier, Set'1 from V, ALICE deposits
/// 1,000 USDT), plus MockLzEndpoint(30101) with the MockOFTAdapter over the
/// USDT, MockLzEndpoint(30110) with MockUSDT0, peered, and B3OftExitFactory
/// pinned to the adapter with DST_EID 30110. One in-process EVM stands for
/// both anvils; LzPacketsV2 plays the harness LayerZero relayer.
contract RelayBatchExitV2Test is RelayFixtureV2 {
    using LzPacketsV2 for LzPacketsV2.Packet;

    uint256 private constant EIP7825_TX_GAS_CAP = 16_777_216;

    uint32 private constant ETH_EID = 30101;
    uint32 private constant ARB_EID = 30110;
    /// Opaque executor options F_x passes through (a type-3 lzReceive gas option).
    bytes private constant EXTRA_OPTIONS = hex"00030100110100000000000000000000000000030d40";
    /// Per-forward relayer fee cap the exit parameters carry (5 USDT).
    uint96 private constant EXIT_FEE_CAP = 5_000_000;
    /// The relayer's fee per forward (1 USDT).
    uint256 private constant FEE = 1_000_000;
    /// Per-item gas cap for forwards: above a first forward (deploys F_x).
    uint256 private constant FORWARD_GAS = 600_000;

    address private constant RELAYER = address(0x5E1A7E01);
    address private constant FEE_SINK = address(0xFEE51C);
    address private constant REFUND_TO = address(0x7EF0D0);

    B3FinalityVerifierV2 private v;
    B3VaultV2 private vault;
    MockLzEndpoint private endpointEth;
    MockLzEndpoint private endpointArb;
    MockOFTAdapter private adapter;
    MockUSDT0 private usdt0;
    B3OftExitFactory private fx;
    /// endpointEth.quote(): the native fee of one forward.
    uint256 private q;

    function setUp() public override {
        super.setUp();
        (v, vault) = deployBridge();
        bootstrap(v, 1, lineageSet(1));
        vm.warp(T0 + REVIEW);
        submit(v, canonicalCert(V_INDEX, 1, lineageSet(1), lineageSet(2), emptyTreeRoot()));
        depositAs(vault, ALICE, 1_000 * UNIT);

        endpointEth = new MockLzEndpoint(ETH_EID);
        endpointArb = new MockLzEndpoint(ARB_EID);
        adapter = new MockOFTAdapter(address(usdt), address(endpointEth));
        usdt0 = new MockUSDT0(address(endpointArb));
        adapter.setPeer(ARB_EID, _word(address(usdt0)));
        usdt0.setPeer(ETH_EID, _word(address(adapter)));
        fx = new B3OftExitFactory(address(usdt), address(adapter), ARB_EID, EXTRA_OPTIONS);
        q = endpointEth.quote();
        assertGt(q, 0);
        vm.deal(RELAYER, 1 ether);

        vm.label(address(vault), "vault");
        vm.label(address(fx), "F_x factory");
        vm.label(address(adapter), "OFT adapter");
        vm.label(address(usdt0), "USDT0");
    }

    // ========================================================== route (d3)

    /// The acceptance case: one call accepts the certificate, releases the
    /// leaf to F_x and forwards F_x through the adapter. The relayer's fee
    /// goes to its fee recipient, the rest is locked in the adapter and
    /// messaged to Arbitrum, the allowance is back to zero, the batch keeps
    /// nothing and writes no slot, and the harness delivery mints USDT0 to
    /// the user once.
    function test_ReleaseToExitForwarderAndForwardInOneCall() public {
        B3ForwarderTypesV2.ExitParams memory p = _exitParams(BOB);
        address account = fx.accountOf(p);
        assertEq(account.code.length, 0, "F_x not yet deployed");
        B3TypesV2.WithdrawalV2[] memory ws = new B3TypesV2.WithdrawalV2[](1);
        ws[0] = withdrawal(0, account, 400 * UNIT, V + 1);
        bytes32[] memory leaves = treeOf(vault, ws);
        Cert memory c11 = canonicalCert(11, V_INDEX, lineageSet(1), lineageSet(2), rootOf(leaves, 1));

        uint256 relayerBefore = RELAYER.balance;
        vm.recordLogs();
        (uint256 certsAccepted, uint256 releasesPaid, uint256 forwardsSent) =
            _relay(inputs(c11), items(item(leaves, 1, ws[0])), _forwards(_fwd(p, FEE, q)), q);
        VmV2.Log[] memory all = vm.getRecordedLogs();

        assertEq(certsAccepted, 1);
        assertEq(releasesPaid, 1);
        assertEq(forwardsSent, 1);
        assertEq(logsOf(all, address(batch)).length, 0, "no failed or skipped item");
        assertTrue(vault.released(0));
        assertEq(vault.locked(), 600 * UNIT);
        assertGt(account.code.length, 0); // deployed on demand
        assertEq(usdt.balanceOf(account), 0, "the forward empties F_x");
        assertEq(usdt.balanceOf(FEE_SINK), FEE, "the F_x fee pays the relayer");
        assertEq(usdt.allowance(account, address(adapter)), 0, "allowance reset");
        assertEq(adapter.lockedBalance(), 400 * UNIT - FEE);
        assertEq(RELAYER.balance, relayerBefore - q, "exactly the quote spent");
        assertEq(REFUND_TO.balance, 0);
        for (uint256 slot = 0; slot < 8; ++slot) {
            assertEq(vm.load(address(batch), bytes32(slot)), bytes32(0));
        }

        LzPacketsV2.Packet memory pk = LzPacketsV2.only(all, address(endpointEth));
        assertEq(uint256(pk.dstEid), ARB_EID);
        assertEq(pk.sender, address(adapter));
        assertEq(pk.to(), _word(BOB));
        assertEq(pk.amount(), 400 * UNIT - FEE);
        pk.deliver(endpointArb);
        assertEq(usdt0.balanceOf(BOB), 400 * UNIT - FEE);
        vm.expectRevert(
            abi.encodeWithSelector(MockLzEndpoint.AlreadyDelivered.selector, ETH_EID, _word(address(adapter)), pk.nonce)
        );
        pk.deliver(endpointArb);
    }

    /// The budget: the factory sends a nativeFee above the quote back to
    /// nativeRefundTo itself, and the batch returns the never-used rest
    /// there; the batch, the factory and F_x end with 0 wei. Without forwards
    /// the whole msg.value comes back.
    function test_UnspentEthIsReturnedAndTheBatchHoldsNothing() public {
        B3ForwarderTypesV2.ExitParams memory p = _exitParams(BOB);
        address account = fx.accountOf(p);
        B3TypesV2.WithdrawalV2[] memory ws = new B3TypesV2.WithdrawalV2[](1);
        ws[0] = withdrawal(0, account, 100 * UNIT, V + 1);
        bytes32[] memory leaves = treeOf(vault, ws);
        submit(v, canonicalCert(11, V_INDEX, lineageSet(1), lineageSet(2), rootOf(leaves, 1)));

        uint256 value = 3 * q + 12_345;
        uint256 relayerBefore = RELAYER.balance;
        (, uint256 releasesPaid, uint256 forwardsSent) =
            _relay(noCerts(), items(item(leaves, 1, ws[0])), _forwards(_fwd(p, FEE, 2 * q)), value);
        assertEq(releasesPaid, 1);
        assertEq(forwardsSent, 1);
        assertEq(RELAYER.balance, relayerBefore - value);
        assertEq(REFUND_TO.balance, value - q, "change from the factory plus the unspent rest");
        assertEq(address(fx).balance, 0);
        assertEq(account.balance, 0);
        assertEq(address(adapter).balance, 0);
        assertEq(address(endpointEth).balance, q, "the endpoint keeps exactly its fee");

        // Nothing to forward: every wei goes back.
        (,, forwardsSent) = _relay(noCerts(), noReleases(), new B3RelayBatch.ExitForward[](0), 7 * q);
        assertEq(forwardsSent, 0);
        assertEq(REFUND_TO.balance, value - q + 7 * q);
    }

    /// The batch counts the budget per call. A nativeRefundTo that re-enters
    /// on receiving the factory's change, with a value-free call naming
    /// another refund target, gets nothing of the outer call's budget; the
    /// outer call still returns its unspent rest to it.
    function test_ReentrantCallCannotTakeAnotherCallsBudget() public {
        address thief = address(0x7E1EF);
        ReentrantRefundeeV2 refundee = new ReentrantRefundeeV2(batch, thief);
        B3ForwarderTypesV2.ExitParams memory p = _exitParams(BOB);
        address account = fx.accountOf(p);
        B3TypesV2.WithdrawalV2[] memory ws = new B3TypesV2.WithdrawalV2[](1);
        ws[0] = withdrawal(0, account, 100 * UNIT, V + 1);
        bytes32[] memory leaves = treeOf(vault, ws);
        submit(v, canonicalCert(11, V_INDEX, lineageSet(1), lineageSet(2), rootOf(leaves, 1)));

        uint256 value = 4 * q;
        vm.prank(RELAYER, RELAYER);
        (, uint256 releasesPaid, uint256 forwardsSent) = batch.relayWithForwards{value: value}(
            v,
            noCerts(),
            CERT_GAS,
            address(vault),
            items(item(leaves, 1, ws[0])),
            RELEASE_GAS,
            address(fx),
            _forwards(_fwd(p, FEE, 2 * q)),
            FORWARD_GAS,
            address(refundee)
        );
        assertEq(releasesPaid, 1);
        assertEq(forwardsSent, 1);
        assertTrue(refundee.reentered(), "the refundee re-entered the batch");
        assertEq(thief.balance, 0, "the inner call returned nothing of the outer budget");
        assertEq(refundee.received(), value - q, "change q plus unspent 2q");
        assertEq(address(refundee).balance, value - q);
        assertBatchEmpty();
    }

    // ========================================================== isolation

    /// Every forward is tried on its own, in order, against what is left of
    /// msg.value (2q): a short native fee, a fee above the cap and invalid
    /// parameters each fail alone and keep their ETH in the budget; an item
    /// asking for more than is left is skipped; the second forward of an
    /// already emptied F_x fails with NothingToForward. Both valid accounts
    /// are forwarded once and delivered once.
    function test_BatchIsolatesFailedForwardsAndSkipsOneOverBudget() public {
        B3ForwarderTypesV2.ExitParams memory pBob = _exitParams(BOB);
        B3ForwarderTypesV2.ExitParams memory pCarol = _exitParams(CAROL);
        B3ForwarderTypesV2.ExitParams memory pBad = _exitParams(DAVE);
        pBad.dstEid = ETH_EID;
        address bobAccount = fx.accountOf(pBob);
        address carolAccount = fx.accountOf(pCarol);
        B3TypesV2.WithdrawalV2[] memory ws = new B3TypesV2.WithdrawalV2[](2);
        ws[0] = withdrawal(0, bobAccount, 300 * UNIT, V + 1);
        ws[1] = withdrawal(1, carolAccount, 200 * UNIT, V + 2);
        bytes32[] memory leaves = treeOf(vault, ws);
        Cert memory c11 = canonicalCert(11, V_INDEX, lineageSet(1), lineageSet(2), rootOf(leaves, 2));

        B3RelayBatch.ExitForward[] memory fw = new B3RelayBatch.ExitForward[](7);
        fw[0] = _fwd(pBob, FEE, q - 1); // NativeFeeShort(q, q - 1); budget stays 2q
        fw[1] = _fwd(pBob, EXIT_FEE_CAP + 1, q); // FeeCapExceeded; budget stays 2q
        fw[2] = _fwd(pCarol, FEE, q); // sent; budget q
        fw[3] = _fwd(pBad, FEE, 0); // InvalidParams
        fw[4] = _fwd(pBob, FEE, 2 * q); // more than is left: skipped
        fw[5] = _fwd(pBob, FEE, q); // sent; budget 0
        fw[6] = _fwd(pCarol, FEE, 0); // emptied by fw[2]: NothingToForward

        uint256 relayerBefore = RELAYER.balance;
        vm.recordLogs();
        (uint256 certsAccepted, uint256 releasesPaid, uint256 forwardsSent) =
            _relay(inputs(c11), items(item(leaves, 2, ws[0]), item(leaves, 2, ws[1])), fw, 2 * q);
        VmV2.Log[] memory all = vm.getRecordedLogs();
        VmV2.Log[] memory logs = logsOf(all, address(batch));

        assertEq(certsAccepted, 1);
        assertEq(releasesPaid, 2);
        assertEq(forwardsSent, 2);
        assertEq(logs.length, 5, "exactly the failed and skipped forwards");
        _assertForwardFailed(
            logs[0], 0, bobAccount, abi.encodeWithSelector(B3OftExitFactory.NativeFeeShort.selector, q, q - 1)
        );
        _assertForwardFailed(logs[1], 1, bobAccount, selectorError(B3OftExitFactory.FeeCapExceeded.selector));
        _assertForwardFailed(logs[2], 3, fx.accountOf(pBad), selectorError(B3OftExitFactory.InvalidParams.selector));
        _assertForwardSkipped(logs[3], 4, bobAccount);
        _assertForwardFailed(logs[4], 6, carolAccount, selectorError(B3OftExitFactory.NothingToForward.selector));

        assertEq(RELAYER.balance, relayerBefore - 2 * q);
        assertEq(REFUND_TO.balance, 0, "the whole budget was spent");
        assertEq(usdt.balanceOf(FEE_SINK), 2 * FEE);
        assertEq(usdt.balanceOf(bobAccount), 0);
        assertEq(usdt.balanceOf(carolAccount), 0);
        assertEq(fx.accountOf(pBad).code.length, 0, "an invalid p is never deployed");

        LzPacketsV2.Packet[] memory packets = LzPacketsV2.sent(all, address(endpointEth));
        assertEq(packets.length, 2);
        assertEq(packets[0].to(), _word(CAROL));
        assertEq(packets[0].amount(), 200 * UNIT - FEE);
        assertEq(packets[1].to(), _word(BOB));
        assertEq(packets[1].amount(), 300 * UNIT - FEE);
        packets[0].deliver(endpointArb);
        packets[1].deliver(endpointArb);
        assertEq(usdt0.balanceOf(CAROL), 200 * UNIT - FEE);
        assertEq(usdt0.balanceOf(BOB), 300 * UNIT - FEE);
    }

    /// Exactly once: the same release and forward relayed again skip the
    /// released leaf and fail the forward with NothingToForward, sending no
    /// packet and returning the whole msg.value. A later withdrawal to the
    /// same (reusable) F_x is released and forwarded once more, paying the
    /// relayer at most feeCap again.
    function test_SecondBatchSkipsTheReleaseAndReportsNothingToForward() public {
        B3ForwarderTypesV2.ExitParams memory p = _exitParams(BOB);
        address account = fx.accountOf(p);
        B3TypesV2.WithdrawalV2[] memory ws = new B3TypesV2.WithdrawalV2[](2);
        ws[0] = withdrawal(0, account, 250 * UNIT, V + 1);
        ws[1] = withdrawal(1, account, 150 * UNIT, heightAt(11) + 1);
        bytes32[] memory leaves = treeOf(vault, ws);
        submit(v, canonicalCert(11, V_INDEX, lineageSet(1), lineageSet(2), rootOf(leaves, 1)));
        B3RelayBatch.ReleaseItem[] memory rel = items(item(leaves, 1, ws[0]));
        B3RelayBatch.ExitForward[] memory fw = _forwards(_fwd(p, FEE, q));

        (, uint256 releasesPaid, uint256 forwardsSent) = _relay(noCerts(), rel, fw, q);
        assertEq(releasesPaid, 1);
        assertEq(forwardsSent, 1);
        uint64 nonceAfterFirst = endpointEth.outboundNonce();

        uint256 refundBefore = REFUND_TO.balance;
        vm.recordLogs();
        (, releasesPaid, forwardsSent) = _relay(noCerts(), rel, fw, q);
        VmV2.Log[] memory all = vm.getRecordedLogs();
        VmV2.Log[] memory logs = logsOf(all, address(batch));
        assertEq(releasesPaid, 0);
        assertEq(forwardsSent, 0);
        assertEq(logs.length, 2);
        assertReleaseSkipped(logs[0], 0, 0);
        _assertForwardFailed(logs[1], 0, account, selectorError(B3OftExitFactory.NothingToForward.selector));
        assertEq(LzPacketsV2.sent(all, address(endpointEth)).length, 0, "no second packet");
        assertEq(endpointEth.outboundNonce(), nonceAfterFirst);
        assertEq(REFUND_TO.balance, refundBefore + q, "the failed forward's ETH came back");
        assertEq(usdt.balanceOf(FEE_SINK), FEE, "one fee for one withdrawal");

        // A second withdrawal to the same F_x: one more release and forward.
        Cert memory c12 = canonicalCert(12, 11, lineageSet(1), lineageSet(2), rootOf(leaves, 2));
        vm.recordLogs();
        (uint256 certsAccepted, uint256 paid, uint256 sent) = _relay(inputs(c12), items(item(leaves, 2, ws[1])), fw, q);
        all = vm.getRecordedLogs();
        assertEq(certsAccepted, 1);
        assertEq(paid, 1);
        assertEq(sent, 1);
        LzPacketsV2.Packet memory pk = LzPacketsV2.only(all, address(endpointEth));
        assertEq(pk.amount(), 150 * UNIT - FEE);
        assertEq(usdt.balanceOf(FEE_SINK), 2 * FEE);
        assertEq(usdt.balanceOf(account), 0);
    }

    /// The factory's own refusals surface as item failures: a fee recipient
    /// equal to F_x itself, and a zero nativeRefundTo with msg.value zero
    /// (the batch refuses it up front only when ETH is sent).
    function test_FactoryRefusalsAreItemFailures() public {
        B3ForwarderTypesV2.ExitParams memory p = _exitParams(BOB);
        address account = fx.accountOf(p);
        B3TypesV2.WithdrawalV2[] memory ws = new B3TypesV2.WithdrawalV2[](1);
        ws[0] = withdrawal(0, account, 100 * UNIT, V + 1);
        bytes32[] memory leaves = treeOf(vault, ws);
        submit(v, canonicalCert(11, V_INDEX, lineageSet(1), lineageSet(2), rootOf(leaves, 1)));

        B3RelayBatch.ExitForward[] memory fw = _forwards(_fwd(p, FEE, 0));
        vm.recordLogs();
        (, uint256 releasesPaid, uint256 forwardsSent) = batch.relayWithForwards(
            v,
            noCerts(),
            CERT_GAS,
            address(vault),
            items(item(leaves, 1, ws[0])),
            RELEASE_GAS,
            address(fx),
            fw,
            FORWARD_GAS,
            address(0)
        );
        VmV2.Log[] memory logs = logsOf(vm.getRecordedLogs(), address(batch));
        assertEq(releasesPaid, 1, "the release still pays");
        assertEq(forwardsSent, 0);
        assertEq(logs.length, 1);
        _assertForwardFailed(logs[0], 0, account, selectorError(B3OftExitFactory.ZeroNativeRefundTo.selector));
        assertEq(usdt.balanceOf(account), 100 * UNIT, "funds wait at F_x");

        fw[0].feeRecipient = account;
        fw[0].nativeFee = q;
        vm.recordLogs();
        (,, forwardsSent) = _relay(noCerts(), noReleases(), fw, q);
        logs = logsOf(vm.getRecordedLogs(), address(batch));
        assertEq(forwardsSent, 0);
        assertEq(logs.length, 1);
        _assertForwardFailed(logs[0], 0, account, selectorError(B3OftExitFactory.BadFeeRecipient.selector));
        assertEq(REFUND_TO.balance, q);

        // A paused endpoint: the send reverts, the funds wait, then go once.
        endpointEth.setPaused(true);
        fw[0].feeRecipient = FEE_SINK;
        vm.recordLogs();
        (,, forwardsSent) = _relay(noCerts(), noReleases(), fw, q);
        logs = logsOf(vm.getRecordedLogs(), address(batch));
        assertEq(forwardsSent, 0);
        assertEq(logs.length, 1);
        _assertForwardFailed(logs[0], 0, account, selectorError(MockLzEndpoint.EndpointPaused.selector));
        assertEq(usdt.balanceOf(account), 100 * UNIT);
        endpointEth.setPaused(false);
        (,, forwardsSent) = _relay(noCerts(), noReleases(), fw, q);
        assertEq(forwardsSent, 1);
        assertEq(usdt.balanceOf(account), 0);
        assertEq(adapter.lockedBalance(), 100 * UNIT - FEE);
    }

    /// A target with code but the wrong interface fails only its own items:
    /// the vault named as exit factory (forward reverts without data and
    /// accountOf answers nothing, so the event names address(0)), while the
    /// certificate and release in the same call apply and every wei returns.
    function test_WrongExitFactoryFailsOnlyItsOwnItems() public {
        B3ForwarderTypesV2.ExitParams memory p = _exitParams(BOB);
        address account = fx.accountOf(p);
        B3TypesV2.WithdrawalV2[] memory ws = new B3TypesV2.WithdrawalV2[](1);
        ws[0] = withdrawal(0, account, 100 * UNIT, V + 1);
        bytes32[] memory leaves = treeOf(vault, ws);
        Cert memory c11 = canonicalCert(11, V_INDEX, lineageSet(1), lineageSet(2), rootOf(leaves, 1));

        vm.recordLogs();
        vm.prank(RELAYER, RELAYER);
        (uint256 certsAccepted, uint256 releasesPaid, uint256 forwardsSent) = batch.relayWithForwards{value: q}(
            v,
            inputs(c11),
            CERT_GAS,
            address(vault),
            items(item(leaves, 1, ws[0])),
            RELEASE_GAS,
            address(vault),
            _forwards(_fwd(p, FEE, q)),
            FORWARD_GAS,
            REFUND_TO
        );
        VmV2.Log[] memory logs = logsOf(vm.getRecordedLogs(), address(batch));
        assertEq(certsAccepted, 1);
        assertEq(releasesPaid, 1);
        assertEq(forwardsSent, 0);
        assertEq(logs.length, 1);
        _assertForwardFailed(logs[0], 0, address(0), "");
        assertEq(REFUND_TO.balance, q);
        assertEq(usdt.balanceOf(account), 100 * UNIT);
        assertBatchEmpty();
    }

    // ============================================================ refusals

    /// Up front, before any item: a codeless exit factory for a non-empty
    /// list (an empty list ignores it), ETH with a zero nativeRefundTo, and a
    /// nativeRefundTo that refuses the unspent rest (RefundFailed reverts the
    /// whole batch, its release included). relay stays non-payable.
    function test_CodelessFactoryZeroRefundToAndFailedRefundRevert() public {
        address codeless = address(0xC0DE1E55);
        B3ForwarderTypesV2.ExitParams memory p = _exitParams(BOB);
        address account = fx.accountOf(p);
        B3TypesV2.WithdrawalV2[] memory ws = new B3TypesV2.WithdrawalV2[](1);
        ws[0] = withdrawal(0, account, 100 * UNIT, V + 1);
        bytes32[] memory leaves = treeOf(vault, ws);
        submit(v, canonicalCert(11, V_INDEX, lineageSet(1), lineageSet(2), rootOf(leaves, 1)));
        B3RelayBatch.ReleaseItem[] memory rel = items(item(leaves, 1, ws[0]));
        B3RelayBatch.ExitForward[] memory fw = _forwards(_fwd(p, FEE, q));
        vm.deal(address(this), 1 ether);

        vm.expectRevert(abi.encodeWithSelector(B3RelayBatch.NoCode.selector, codeless));
        batch.relayWithForwards{value: q}(
            v, noCerts(), CERT_GAS, address(vault), rel, RELEASE_GAS, codeless, fw, FORWARD_GAS, REFUND_TO
        );

        vm.expectRevert(B3RelayBatch.ZeroNativeRefundTo.selector);
        batch.relayWithForwards{value: q}(
            v, noCerts(), CERT_GAS, address(vault), rel, RELEASE_GAS, address(fx), fw, FORWARD_GAS, address(0)
        );

        // MockTetherToken has no receive: the unspent q cannot go back.
        vm.expectRevert(B3RelayBatch.RefundFailed.selector);
        batch.relayWithForwards{value: q}(
            v,
            noCerts(),
            CERT_GAS,
            address(vault),
            rel,
            RELEASE_GAS,
            codeless,
            new B3RelayBatch.ExitForward[](0),
            FORWARD_GAS,
            address(usdt)
        );
        assertFalse(vault.released(0), "the failed refund reverted the release too");

        // An empty forward list ignores a codeless exit factory; no ETH, no refund.
        (uint256 certsAccepted, uint256 releasesPaid, uint256 forwardsSent) = batch.relayWithForwards(
            v,
            noCerts(),
            CERT_GAS,
            address(vault),
            rel,
            RELEASE_GAS,
            codeless,
            new B3RelayBatch.ExitForward[](0),
            FORWARD_GAS,
            address(0)
        );
        assertEq(certsAccepted, 0);
        assertEq(releasesPaid, 1);
        assertEq(forwardsSent, 0);
        assertEq(usdt.balanceOf(account), 100 * UNIT);
        assertBatchEmpty();

        bytes memory relayCall =
            abi.encodeCall(B3RelayBatch.relay, (v, noCerts(), CERT_GAS, address(vault), noReleases(), RELEASE_GAS));
        (bool ok,) = address(batch).call{value: 1}(relayCall);
        assertFalse(ok, "relay is not payable");
    }

    // ================================================================ gas

    /// One release plus the first forward (F_x deployed on demand) and, for
    /// a second withdrawal to the same F_x, one release plus a later forward,
    /// each cold in one batch: logged for the spec §11 (d3) row and bounded by
    /// the per-item caps.
    function test_ReleasePlusForwardGas() public {
        B3ForwarderTypesV2.ExitParams memory p = _exitParams(BOB);
        address account = fx.accountOf(p);
        B3TypesV2.WithdrawalV2[] memory ws = new B3TypesV2.WithdrawalV2[](2);
        ws[0] = withdrawal(0, account, 250 * UNIT, V + 1);
        ws[1] = withdrawal(1, account, 150 * UNIT, V + 2);
        bytes32[] memory leaves = treeOf(vault, ws);
        submit(v, canonicalCert(11, V_INDEX, lineageSet(1), lineageSet(2), rootOf(leaves, 2)));
        B3RelayBatch.ExitForward[] memory fw = _forwards(_fwd(p, FEE, q));
        vm.deal(address(this), 1 ether);

        for (uint64 id = 0; id < 2; ++id) {
            bytes memory data = abi.encodeCall(
                B3RelayBatch.relayWithForwards,
                (
                    v,
                    noCerts(),
                    CERT_GAS,
                    address(vault),
                    items(item(leaves, 2, ws[id])),
                    RELEASE_GAS,
                    address(fx),
                    fw,
                    FORWARD_GAS,
                    REFUND_TO
                )
            );
            _coolAll(v, vault, account);
            uint256 g0 = gasleft();
            (bool ok, bytes memory ret) = address(batch).call{value: q}(data);
            uint256 execution = g0 - gasleft();
            assertTrue(ok, "batch call");
            (, uint256 releasesPaid, uint256 forwardsSent) = abi.decode(ret, (uint256, uint256, uint256));
            assertEq(releasesPaid, 1);
            assertEq(forwardsSent, 1);
            assertLt(execution, RELEASE_GAS + FORWARD_GAS, "within the per-item caps");
            ConsoleV2.log(
                id == 0
                    ? "batch release + first forward (deploys F_x): execution gas"
                    : "batch release + later forward: execution gas",
                execution
            );
            ConsoleV2.log("  conservative gas", 21_000 + data.length * 40 + execution);
        }
        assertEq(usdt.balanceOf(FEE_SINK), 2 * FEE);
        assertBatchEmpty();
    }

    /// One 64-member transition certificate (43 of 64 signing, 21 absent
    /// witnesses), one release to a fresh F_x and its first forward (deploy,
    /// fee, approvals, OFT send), cold, in one batch: 21,000 + 40 gas per
    /// calldata byte + execution stays under 2^24 (EIP-7825).
    function test_SixtyFourMemberTransitionPlusReleasePlusForwardFitsTheCap() public {
        Signer.TestSet memory big1 =
            Signer.makeUniformSet(harness, 1, keccak256("RelayBatchExitV2Test/64-member Set'1"), 64, 100);
        Signer.TestSet memory s2 = lineageSet(2);
        (B3FinalityVerifierV2 v64, B3VaultV2 vault64) = deployBridge();
        bootstrap(v64, 1, big1);
        vm.warp(block.timestamp + MIN_EPOCH);

        // Reserve by plain transfer and absorb (no deposit needed for a release).
        usdt.issue(500 * UNIT);
        usdt.transfer(address(vault64), 500 * UNIT);
        assertEq(vault64.absorb(), 500 * UNIT);

        B3ForwarderTypesV2.ExitParams memory p = _exitParams(address(0xFEE0FEE0));
        address account = fx.accountOf(p);
        B3TypesV2.WithdrawalV2[] memory ws = new B3TypesV2.WithdrawalV2[](1);
        ws[0] = withdrawal(0, account, 500 * UNIT, V);
        bytes32[] memory leaves = treeOf(vault64, ws);
        uint32[] memory absent = Signer.minQuorumAbsent(64);
        assertEq(absent.length, 21);
        Cert memory c = certWithRoot(CANONICAL, V_INDEX, 1, big1, s2, rootOf(leaves, 1), 0, absent);

        bytes memory data = abi.encodeCall(
            B3RelayBatch.relayWithForwards,
            (
                v64,
                inputs(c),
                CERT_GAS,
                address(vault64),
                items(item(leaves, 1, ws[0])),
                RELEASE_GAS,
                address(fx),
                _forwards(_fwd(p, FEE, q)),
                FORWARD_GAS,
                REFUND_TO
            )
        );
        vm.deal(address(this), 1 ether);
        vm.recordLogs();
        _coolAll(v64, vault64, account);
        uint256 g0 = gasleft();
        (bool ok, bytes memory ret) = address(batch).call{value: q}(data);
        uint256 execution = g0 - gasleft();
        assertTrue(ok, "batch call");
        (uint256 certsAccepted, uint256 releasesPaid, uint256 forwardsSent) =
            abi.decode(ret, (uint256, uint256, uint256));
        assertEq(certsAccepted, 1);
        assertEq(releasesPaid, 1);
        assertEq(forwardsSent, 1);
        assertEq(uint256(v64.currentEpoch()), 1);
        assertEq(v64.currentSetHash(), big1.setHash);
        assertEq(usdt.balanceOf(account), 0);
        assertEq(usdt.balanceOf(FEE_SINK), FEE);
        LzPacketsV2.Packet memory pk = LzPacketsV2.only(vm.getRecordedLogs(), address(endpointEth));
        assertEq(pk.amount(), 500 * UNIT - FEE);
        assertBatchEmpty();

        uint256 conservative = 21_000 + data.length * 40 + execution;
        ConsoleV2.log("batch (64-member cert + release + first forward): calldata bytes", data.length);
        ConsoleV2.log("batch (64-member cert + release + first forward): execution gas", execution);
        ConsoleV2.log("batch (64-member cert + release + first forward): conservative gas", conservative);
        assertLt(conservative, EIP7825_TX_GAS_CAP, "EIP-7825 cap");
    }

    // ============================================================ helpers

    function _word(address a) private pure returns (bytes32) {
        return bytes32(uint256(uint160(a)));
    }

    /// A withdrawal to `to` on Arbitrum: 0.5% slippage bound, 5 USDT fee cap,
    /// ALICE as refundEOA.
    function _exitParams(address to) private pure returns (B3ForwarderTypesV2.ExitParams memory) {
        return B3ForwarderTypesV2.ExitParams({
            dstEid: ARB_EID, to: _word(to), maxBridgeFeeBps: 50, feeCap: EXIT_FEE_CAP, refundEOA: ALICE
        });
    }

    function _fwd(B3ForwarderTypesV2.ExitParams memory p, uint256 fee, uint256 nativeFee)
        private
        pure
        returns (B3RelayBatch.ExitForward memory)
    {
        return B3RelayBatch.ExitForward({p: p, fee: fee, feeRecipient: FEE_SINK, nativeFee: nativeFee});
    }

    function _forwards(B3RelayBatch.ExitForward memory a)
        private
        pure
        returns (B3RelayBatch.ExitForward[] memory list)
    {
        list = new B3RelayBatch.ExitForward[](1);
        list[0] = a;
    }

    /// relayWithForwards from RELAYER (a top-level EOA) with `value`, the
    /// fixture's gas caps and REFUND_TO; checks the batch keeps nothing.
    function _relay(
        B3RelayBatch.CertInput[] memory certs,
        B3RelayBatch.ReleaseItem[] memory releases,
        B3RelayBatch.ExitForward[] memory forwards,
        uint256 value
    ) private returns (uint256 certsAccepted, uint256 releasesPaid, uint256 forwardsSent) {
        vm.prank(RELAYER, RELAYER);
        (certsAccepted, releasesPaid, forwardsSent) = batch.relayWithForwards{value: value}(
            v, certs, CERT_GAS, address(vault), releases, RELEASE_GAS, address(fx), forwards, FORWARD_GAS, REFUND_TO
        );
        assertBatchEmpty();
    }

    function _assertForwardFailed(VmV2.Log memory log, uint256 index, address account, bytes memory reason)
        private
        pure
    {
        assertEq(log.topics.length, 3);
        assertEq(log.topics[0], B3RelayBatch.ForwardFailed.selector);
        assertEq(log.topics[1], bytes32(index));
        assertEq(log.topics[2], _word(account));
        assertEq(log.data, abi.encode(reason));
    }

    function _assertForwardSkipped(VmV2.Log memory log, uint256 index, address account) private pure {
        assertEq(log.topics.length, 3);
        assertEq(log.topics[0], B3RelayBatch.ForwardSkipped.selector);
        assertEq(log.topics[1], bytes32(index));
        assertEq(log.topics[2], _word(account));
        assertEq(log.data.length, 0);
    }

    function _coolAll(B3FinalityVerifierV2 target, B3VaultV2 targetVault, address account) private {
        vm.cool(address(target));
        vm.cool(address(prover));
        vm.cool(address(targetVault));
        vm.cool(address(usdt));
        vm.cool(address(batch));
        vm.cool(address(fx));
        vm.cool(address(adapter));
        vm.cool(address(endpointEth));
        vm.cool(account);
    }
}
