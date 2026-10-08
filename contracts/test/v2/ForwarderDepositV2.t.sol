// SPDX-License-Identifier: MIT
pragma solidity ^0.8.28;

import {ConsoleV2, VmV2} from "./V2TestBase.sol";
import {ForwarderFixtureV2} from "./utils/ForwarderFixtureV2.sol";
import {ReceiptSizeV2} from "./utils/ReceiptSizeV2.sol";
import {RuntimeScanV2 as Scan} from "./utils/RuntimeScanV2.sol";
import {SafeLikeCallerV2} from "./mocks/VaultMocksV2.sol";
import {B3VaultV2} from "../../src/v2/B3VaultV2.sol";
import {IB3FinalityVerifierV2} from "../../src/v2/IB3FinalityVerifierV2.sol";
import {B3Forwarder} from "../../src/v2/B3Forwarder.sol";
import {B3ForwarderTypesV2} from "../../src/v2/B3ForwarderTypesV2.sol";
import {B3DepositForwarderFactory} from "../../src/v2/B3DepositForwarderFactory.sol";

/// A foreign token whose transfer re-enters the factory (test only).
contract ReenteringTokenFdV2 {
    B3DepositForwarderFactory private immutable factory;

    constructor(B3DepositForwarderFactory factory_) {
        factory = factory_;
    }

    function transfer(address, uint256) external returns (bool) {
        factory.collectFee(msg.sender, msg.sender);
        return true;
    }
}

/// B3DepositForwarderFactory and B3Forwarder for route (b) (contracts spec §7,
/// §10, §12; regtest plan RT6): address derivation, Published, PR-1 (a flush
/// emits only the token Transfer and the vault Deposit, a receipt identical
/// in shape to a route (a) deposit, within the leaf budget), PR-2 (only a
/// codeless EOA's top-level call flushes), mocked OFT credit before
/// deployment, flushing exactly once and in MAX_DEPOSIT_RAW parts, OI-2 fee
/// caps, D5, notAfter, the refund and returnToOwner escapes, closed deposits,
/// a blacklisted account, fee collection, a fuzz over D2-D6 and the
/// runtime rules.
contract ForwarderDepositV2Test is ForwarderFixtureV2 {
    uint256 private constant MIN_FLUSH = 1_000_000;

    // =========================================================== derivation

    function test_TagsMatchTheirStrings() public pure {
        assertEq(B3ForwarderTypesV2.DEPOSIT_TAG, keccak256("B3/FWD/DEPOSIT/V2"));
        assertEq(B3ForwarderTypesV2.EXIT_TAG, keccak256("B3/FWD/EXIT/OFT/V2"));
    }

    function test_InitCodeHashIsTheForwarderCreationCode() public view {
        bytes32 expected = keccak256(type(B3Forwarder).creationCode);
        assertEq(fd.FORWARDER_INIT_CODE_HASH(), expected);
        assertEq(fx.FORWARDER_INIT_CODE_HASH(), expected, "both factories deploy the same init code");
    }

    function test_FactoryPinsTheVault() public view {
        assertEq(fd.VAULT(), address(vault));
        assertEq(fd.TOKEN(), address(usdt));
        assertEq(fd.MAX_DEPOSIT_RAW(), CAP);
        assertEq(fd.MIN_FLUSH_RAW(), MIN_FLUSH);
        assertEq(vault.FORWARDER_FACTORY(), address(fd));
    }

    function test_FactoryConstructorRefusesBadVault() public {
        vm.expectRevert(B3DepositForwarderFactory.BadConfig.selector);
        new B3DepositForwarderFactory(address(vault), bytes32(0));
        vm.expectRevert(B3DepositForwarderFactory.BadConfig.selector);
        new B3DepositForwarderFactory(address(vault), keccak256("another runtime"));
        vm.expectRevert(B3DepositForwarderFactory.BadConfig.selector);
        new B3DepositForwarderFactory(address(0xC0DE1E55), address(0xC0DE1E55).codehash);
        // The vault names `fd` as its factory, not this new one.
        vm.expectRevert(B3DepositForwarderFactory.BadConfig.selector);
        new B3DepositForwarderFactory(address(vault), address(vault).codehash);

        // A vault whose cap is below MIN_FLUSH_RAW could never be flushed into.
        address predicted = CHEATS.computeCreateAddress(address(this), uint256(CHEATS.getNonce(address(this))) + 1);
        B3VaultV2 small = _vault(predicted, MIN_FLUSH - 1);
        vm.expectRevert(B3DepositForwarderFactory.BadConfig.selector);
        new B3DepositForwarderFactory(address(small), address(small).codehash);
    }

    /// accountOf is the CREATE2 address of B3Forwarder for the 192-byte salt
    /// preimage, and deploy creates the account exactly there.
    function test_AccountOfIsCreate2AndTheDeployment() public {
        B3ForwarderTypesV2.DepositParams memory p = depositParams();
        bytes memory preimage = B3ForwarderTypesV2.depositSaltPreimage(p);
        assertEq(preimage.length, 192);
        assertEq(
            preimage,
            abi.encode(
                keccak256("B3/FWD/DEPOSIT/V2"),
                p.vault,
                p.b3Recipient,
                p.refundEOA,
                uint256(p.feeCap),
                uint256(p.notAfter)
            )
        );
        bytes32 salt = keccak256(preimage);
        assertEq(B3ForwarderTypesV2.depositSalt(p), salt);
        bytes memory create2Preimage =
            abi.encodePacked(bytes1(0xff), address(fd), salt, keccak256(type(B3Forwarder).creationCode));
        assertEq(create2Preimage.length, 85);
        address expected = address(uint160(uint256(keccak256(create2Preimage))));
        assertEq(fd.accountOf(p), expected);
        assertEq(B3ForwarderTypesV2.create2Address(address(fd), salt, fd.FORWARDER_INIT_CODE_HASH()), expected);
        assertEq(expected.code.length, 0);

        vm.prank(RELAYER);
        assertEq(fd.deploy(p), expected);
        assertGt(expected.code.length, 0);
        assertEq(B3Forwarder(expected).FACTORY(), address(fd));
        assertTrue(fd.isAccount(expected));
        assertEq(usdt.allowance(expected, address(vault)), type(uint256).max);

        // Every field moves the address.
        B3ForwarderTypesV2.DepositParams memory q = depositParams();
        q.b3Recipient = bytes32(uint256(B3_RECIPIENT) + 1);
        assertTrue(fd.accountOf(q) != expected);
        q = depositParams();
        q.refundEOA = BOB;
        assertTrue(fd.accountOf(q) != expected);
        q = depositParams();
        q.feeCap += 1;
        assertTrue(fd.accountOf(q) != expected);
        q = depositParams();
        q.notAfter += 1;
        assertTrue(fd.accountOf(q) != expected);
        q = depositParams();
        q.vault = address(0xBAD);
        assertTrue(fd.accountOf(q) != expected);
    }

    // ============================================================ lifecycle

    function test_DeployAndPublishEmitPublished() public {
        B3ForwarderTypesV2.DepositParams memory p = depositParams();
        address account = fd.accountOf(p);

        vm.expectEmit(true, true, true, true, address(fd));
        emit B3DepositForwarderFactory.Published(account, p);
        vm.prank(BOB);
        assertEq(fd.publish(p), account);
        assertEq(account.code.length, 0, "publish deploys nothing");
        assertFalse(fd.isAccount(account));

        vm.expectEmit(true, true, true, true, address(fd));
        emit B3DepositForwarderFactory.Published(account, p);
        vm.prank(BOB);
        assertEq(fd.deploy(p), account);
        assertTrue(fd.isAccount(account));

        vm.expectRevert(B3DepositForwarderFactory.AlreadyDeployed.selector);
        fd.deploy(p);
        // Publishing stays possible (no state).
        vm.expectEmit(true, true, true, true, address(fd));
        emit B3DepositForwarderFactory.Published(account, p);
        fd.publish(p);
    }

    function test_InvalidParamsRefusedByDeployFlushPublishButNotRefund() public {
        B3ForwarderTypesV2.DepositParams[] memory bad = new B3ForwarderTypesV2.DepositParams[](8);
        bytes4[] memory errs = new bytes4[](8);
        for (uint256 i = 0; i < bad.length; ++i) {
            bad[i] = depositParams();
        }
        bad[0].vault = address(0xBAD);
        errs[0] = B3DepositForwarderFactory.WrongVault.selector;
        bad[1].b3Recipient = bytes32(uint256(uint160(0x5151515151515151515151515151515151515151))); // version 0
        errs[1] = B3DepositForwarderFactory.BadB3Recipient.selector;
        bad[2].b3Recipient = bytes32(uint256(B3_RECIPIENT) | (uint256(2) << 160)); // version 3
        errs[2] = B3DepositForwarderFactory.BadB3Recipient.selector;
        bad[3].b3Recipient = bytes32(uint256(1) << 160); // zero key hash
        errs[3] = B3DepositForwarderFactory.BadB3Recipient.selector;
        bad[4].refundEOA = address(0);
        errs[4] = B3DepositForwarderFactory.BadRefundEOA.selector;
        bad[5].refundEOA = address(vault);
        errs[5] = B3DepositForwarderFactory.BadRefundEOA.selector;
        bad[6].refundEOA = address(usdt);
        errs[6] = B3DepositForwarderFactory.BadRefundEOA.selector;
        bad[7].refundEOA = address(fd);
        errs[7] = B3DepositForwarderFactory.BadRefundEOA.selector;

        for (uint256 i = 0; i < bad.length; ++i) {
            assertFalse(fd.isValid(bad[i]));
            vm.expectRevert(errs[i]);
            fd.deploy(bad[i]);
            vm.expectRevert(errs[i]);
            fd.publish(bad[i]);
            expectFlushRevert(RELAYER, bad[i], 0, errs[i]);
            vm.expectRevert(errs[i]);
            fd.returnToOwner(bad[i], 0);
        }
        assertTrue(fd.isValid(depositParams()));

        // Tokens sent to an invalid p's account still go back to its refundEOA.
        for (uint256 i = 0; i < 4; ++i) {
            address account = fd.accountOf(bad[i]);
            fundUsdt(account, 3 * UNIT);
            vm.prank(ALICE);
            fd.refund(bad[i], address(usdt), CAROL, 3 * UNIT);
            assertEq(usdt.balanceOf(account), 0);
            assertTrue(fd.isAccount(account), "refund deploys on demand");
            // ...and the account can still never flush.
            expectFlushRevert(RELAYER, bad[i], 0, errs[i]);
        }
        assertEq(usdt.balanceOf(CAROL), 12 * UNIT);
    }

    // ======================================================= route (b), PR-1

    /// Route (b): USDT0 from Arbitrum reaches the undeployed F_d by the
    /// adapter's plain transfer; a relayer deploys and flushes it.
    function test_RouteB_MockedOftCreditBeforeDeployThenFlush() public {
        B3ForwarderTypesV2.DepositParams memory p = depositParams();
        address account = fd.accountOf(p);
        uint256 amount = 500 * UNIT;

        bridgeFromArbitrum(BOB, account, amount);
        assertEq(account.code.length, 0, "credited before deployment");
        assertEq(usdt.balanceOf(account), amount);
        assertEq(usdt0.balanceOf(BOB), 0);
        assertEq(usdt0.totalSupply(), 0, "burned on Arbitrum");
        expectFlushRevert(RELAYER, p, FEE, B3DepositForwarderFactory.NotDeployed.selector);

        vm.prank(RELAYER, RELAYER);
        fd.deploy(p);
        vm.expectEmit(true, true, true, true, address(vault));
        emit B3VaultV2.Deposit(0, address(usdt), amount - FEE, B3_RECIPIENT);
        assertEq(flushAs(RELAYER, p, FEE), amount - FEE);

        assertEq(vault.locked(), amount - FEE);
        assertEq(usdt.balanceOf(address(vault)), amount - FEE);
        assertEq(usdt.balanceOf(account), FEE);
        assertEq(fd.reserved(account), FEE);
        assertEq(fd.feesCharged(account), FEE);
        assertEq(fd.owed(account, RELAYER), FEE);
        assertEq(fd.freeBalance(account), 0);

        vm.prank(RELAYER2);
        assertEq(fd.collectFee(account, RELAYER), FEE);
        assertEq(usdt.balanceOf(RELAYER), FEE);
        assertEq(usdt.balanceOf(account), 0);
    }

    /// PR-1 / D1: the flush receipt carries exactly the token Transfer
    /// (account -> vault) and the vault's V1 Deposit event.
    function test_PR1_FlushEmitsOnlyTransferAndDeposit() public {
        (B3ForwarderTypesV2.DepositParams memory p, address account) = _deployedFunded(500 * UNIT);

        vm.recordLogs();
        flushAs(RELAYER, p, FEE);
        VmV2.Log[] memory logs = vm.getRecordedLogs();

        assertEq(logs.length, 2, "exactly two logs");
        assertEq(logs[0].emitter, address(usdt));
        assertEq(logs[0].topics.length, 3);
        assertEq(logs[0].topics[0], TRANSFER_TOPIC);
        assertEq(logs[0].topics[1], word(account));
        assertEq(logs[0].topics[2], word(address(vault)));
        assertEq(logs[0].data, abi.encode(500 * UNIT - FEE));

        assertEq(logs[1].emitter, address(vault));
        assertEq(logs[1].topics.length, 3);
        assertEq(logs[1].topics[0], DEPOSIT_TOPIC);
        assertEq(logs[1].topics[1], bytes32(0), "deposit id 0");
        assertEq(logs[1].topics[2], word(address(usdt)));
        assertEq(logs[1].data, abi.encode(500 * UNIT - FEE, B3_RECIPIENT));
    }

    /// PR-1: a flush's receipt is a route (a) deposit's receipt with the
    /// Transfer's `from` topic changed from the EOA to the account; the
    /// Deposit log is byte-identical and the leaf has the same size.
    function test_PR1_FlushReceiptMatchesRouteADeposit() public {
        uint256 amount = 750 * UNIT;
        (B3ForwarderTypesV2.DepositParams memory p,) = _deployedFunded(amount + FEE);
        fundUsdt(ALICE, amount);
        vm.prank(ALICE);
        usdt.approve(address(vault), amount);

        uint256 snapshot = vm.snapshotState();
        vm.recordLogs();
        vm.prank(ALICE, ALICE);
        vault.deposit(amount, B3_RECIPIENT);
        VmV2.Log[] memory routeA = vm.getRecordedLogs();
        assertTrue(vm.revertToState(snapshot));

        vm.recordLogs();
        flushAs(RELAYER, p, FEE);
        VmV2.Log[] memory flush = vm.getRecordedLogs();

        assertEq(routeA.length, 2);
        assertEq(flush.length, 2);
        for (uint256 i = 0; i < 2; ++i) {
            assertEq(flush[i].emitter, routeA[i].emitter);
            assertEq(flush[i].topics.length, routeA[i].topics.length);
            assertEq(flush[i].data, routeA[i].data);
            for (uint256 t = 0; t < flush[i].topics.length; ++t) {
                if (i == 0 && t == 1) continue; // Transfer `from`
                assertEq(flush[i].topics[t], routeA[i].topics[t]);
            }
        }
        assertEq(routeA[0].topics[1], word(ALICE));
        assertEq(flush[0].topics[1], word(fd.accountOf(p)));
        assertEq(ReceiptSizeV2.leafSize(flush), ReceiptSizeV2.leafSize(routeA));
    }

    /// Spec §7.1 / PR-3: the worst-case receipt-trie leaf of a flush is far
    /// inside the relayer's 7,168-byte skip limit and the modelled 1,133.
    ///
    /// By hand: Transfer log 157 bytes (address 21, topics 101, data 33,
    /// header 2), Deposit log 190 (21 + 101 + data 66 + 2), log list 350;
    /// receipt body 1 + 9 + 259 + 350 = 619, list 622, typed 623; leaf =
    /// rlp([path 5, value 626]) = 634.
    function test_PR3_FlushReceiptLeafWithinBudget() public {
        (B3ForwarderTypesV2.DepositParams memory p,) = _deployedFunded(CAP + FEE);
        vm.recordLogs();
        flushAs(RELAYER, p, FEE);
        VmV2.Log[] memory logs = vm.getRecordedLogs();

        uint256 size = ReceiptSizeV2.leafSize(logs);
        ConsoleV2.log("F_d flush receipt leaf bytes (worst case)", size);
        assertEq(size, 634);
        assertLe(size, ReceiptSizeV2.MODELLED_MAX, "within the modelled 1,133");
        assertLe(size, ReceiptSizeV2.SKIP_LIMIT, "PR-3: a relayer flushes it");
        assertLe(ReceiptSizeV2.SKIP_LIMIT, ReceiptSizeV2.TERMINAL_NODE_LIMIT);
    }

    /// The size model on hand-computed receipts.
    function test_ReceiptSizeModelHandComputed() public pure {
        VmV2.Log[] memory none = new VmV2.Log[](0);
        // body 1 + 9 + 259 + 1 = 270, list 273, typed 274; leaf rlp([5, 277]) = 285.
        assertEq(ReceiptSizeV2.leafSize(none), 285);
        assertEq(ReceiptSizeV2.typedReceipt(none).length, 274);

        VmV2.Log[] memory one = new VmV2.Log[](1);
        one[0] = VmV2.Log({topics: new bytes32[](0), data: "", emitter: address(0xE)});
        // log rlp([addr 21, 0xc0, 0x80]) = 24 bytes; body 294, list 297, typed 298; leaf rlp([5, 301]) = 309.
        assertEq(ReceiptSizeV2.encodeLog(one[0]).length, 24);
        assertEq(ReceiptSizeV2.leafSize(one), 309);
    }

    /// PR-3 is what a wrapper with junk logs would trip; PR-2 makes the
    /// wrapper impossible in the first place (see the PR-2 tests).
    function test_PR3_JunkLogsWouldExceedTheSkipLimit() public {
        (B3ForwarderTypesV2.DepositParams memory p,) = _deployedFunded(500 * UNIT);
        vm.recordLogs();
        flushAs(RELAYER, p, FEE);
        VmV2.Log[] memory flush = vm.getRecordedLogs();

        VmV2.Log[] memory wrapped = new VmV2.Log[](8);
        wrapped[0] = flush[0];
        wrapped[1] = flush[1];
        for (uint256 i = 2; i < wrapped.length; ++i) {
            wrapped[i] = VmV2.Log({topics: new bytes32[](4), data: new bytes(1_100), emitter: address(0xBAD)});
        }
        assertGt(ReceiptSizeV2.leafSize(wrapped), ReceiptSizeV2.SKIP_LIMIT);
    }

    // ================================================================= PR-2

    function test_PR2_ContractCallerRefused() public {
        (B3ForwarderTypesV2.DepositParams memory p,) = _deployedFunded(500 * UNIT);
        SafeLikeCallerV2 safe = new SafeLikeCallerV2();
        vm.expectRevert(B3DepositForwarderFactory.BadCaller.selector);
        vm.prank(RELAYER, RELAYER);
        safe.exec(address(fd), abi.encodeCall(B3DepositForwarderFactory.flush, (p, FEE)));
    }

    function test_PR2_Eip7702OriginRefused() public {
        (B3ForwarderTypesV2.DepositParams memory p,) = _deployedFunded(500 * UNIT);
        SafeLikeCallerV2 implementation = new SafeLikeCallerV2();
        vm.etch(RELAYER, abi.encodePacked(hex"ef0100", address(implementation)));
        assertEq(RELAYER.code.length, 23);
        expectFlushRevert(RELAYER, p, FEE, B3DepositForwarderFactory.BadCaller.selector);
    }

    function test_PR2_SenderOtherThanOriginRefused() public {
        (B3ForwarderTypesV2.DepositParams memory p, address account) = _deployedFunded(500 * UNIT);
        vm.expectRevert(B3DepositForwarderFactory.BadCaller.selector);
        vm.prank(RELAYER, ALICE);
        fd.flush(p, FEE);
        // The test contract itself is a contract caller.
        vm.expectRevert(B3DepositForwarderFactory.BadCaller.selector);
        fd.flush(p, FEE);
        assertEq(usdt.balanceOf(account), 500 * UNIT);
        assertEq(flushAs(RELAYER, p, FEE), 500 * UNIT - FEE, "a codeless EOA flushes");
    }

    // ======================================================== exactly once

    function test_FlushExactlyOnce() public {
        (B3ForwarderTypesV2.DepositParams memory p, address account) = _deployedFunded(500 * UNIT);
        assertEq(flushAs(RELAYER, p, FEE), 500 * UNIT - FEE);
        assertEq(vault.nextDepositId(), 1);

        expectFlushRevert(RELAYER, p, 0, B3DepositForwarderFactory.BelowMinimum.selector);
        expectFlushRevert(RELAYER, p, FEE, B3DepositForwarderFactory.InsufficientBalance.selector);
        expectFlushRevert(RELAYER2, p, 0, B3DepositForwarderFactory.BelowMinimum.selector);
        assertEq(vault.nextDepositId(), 1, "one Deposit");
        assertEq(usdt.balanceOf(account), FEE, "only the accrued fee is left");
    }

    function test_FlushBelowMinimumRefusedThenRefunded() public {
        (B3ForwarderTypesV2.DepositParams memory p,) = _deployedFunded(MIN_FLUSH - 1);
        expectFlushRevert(RELAYER, p, 0, B3DepositForwarderFactory.BelowMinimum.selector);
        // The fee counts against the amount: 1 USDT + fee is the smallest paid flush.
        fundUsdt(fd.accountOf(p), 1);
        assertEq(flushAs(RELAYER, p, 0), MIN_FLUSH);

        fundUsdt(fd.accountOf(p), MIN_FLUSH);
        expectFlushRevert(RELAYER, p, 1, B3DepositForwarderFactory.BelowMinimum.selector);
        vm.prank(ALICE);
        fd.refund(p, address(usdt), CAROL, MIN_FLUSH);
        assertEq(usdt.balanceOf(CAROL), MIN_FLUSH);
    }

    /// D6: each flush deposits min(free - fee, MAX_DEPOSIT_RAW); the caller
    /// picks only the fee.
    function test_FlushSplitsAtMaxDepositRaw() public {
        (B3ForwarderTypesV2.DepositParams memory p, address account) = _deployedFunded(25_000 * UNIT);
        assertEq(flushAs(RELAYER, p, FEE), CAP);
        assertEq(flushAs(RELAYER, p, FEE), CAP);
        assertEq(fd.freeBalance(account), 5_000 * UNIT - 2 * FEE);
        assertEq(flushAs(RELAYER, p, FEE), 5_000 * UNIT - 3 * FEE);
        expectFlushRevert(RELAYER, p, 0, B3DepositForwarderFactory.BelowMinimum.selector);

        assertEq(vault.nextDepositId(), 3);
        assertEq(vault.locked(), 25_000 * UNIT - 3 * FEE);
        assertEq(usdt.balanceOf(account), 3 * FEE);
        assertEq(fd.reserved(account), 3 * FEE);
    }

    // ================================================================= OI-2

    /// OI-2 (chosen: the wallet caps a route (b) delivery at MAX_DEPOSIT_RAW
    /// per forwarder and sets feeCap = 4 x fee). Even a greedy first flusher
    /// that takes the whole lifetime cap at once, front-running the relayer
    /// that paid for the deploy, deposits all of such a delivery: nothing is
    /// stranded. The deploying relayer goes unpaid; that is relayer
    /// economics, not user funds.
    function test_OI2_GreedyFirstFlushStillMovesADeliveryUpToMaxDepositRaw() public {
        B3ForwarderTypesV2.DepositParams memory p = depositParams();
        assertEq(uint256(p.feeCap), 4 * FEE);
        address account = fd.accountOf(p);
        bridgeFromArbitrum(BOB, account, CAP);
        vm.prank(RELAYER, RELAYER);
        fd.deploy(p);

        assertEq(flushAs(RELAYER2, p, p.feeCap), CAP - 4 * FEE, "the whole delivery less the whole cap");
        assertEq(fd.freeBalance(account), 0, "nothing is stranded");
        assertEq(vault.locked(), CAP - 4 * FEE);
        expectFlushRevert(RELAYER, p, FEE, B3DepositForwarderFactory.FeeCapExceeded.selector);
        assertEq(fd.owed(account, RELAYER), 0);
        assertEq(fd.collectFee(account, RELAYER2), 4 * FEE);

        // A repeat arrival then moves only at zero fee, or by refund.
        bridgeFromArbitrum(BOB, account, 100 * UNIT);
        expectFlushRevert(RELAYER, p, 1, B3DepositForwarderFactory.FeeCapExceeded.selector);
        uint256 snapshot = vm.snapshotState();
        assertEq(flushAs(ALICE, p, 0), 100 * UNIT);
        assertTrue(vm.revertToState(snapshot));
        vm.prank(ALICE);
        fd.refund(p, address(usdt), CAROL, 100 * UNIT);
        assertEq(usdt.balanceOf(CAROL), 100 * UNIT);
        assertEq(usdt.balanceOf(account), 0);
    }

    /// Why the wallet refuses a larger delivery. With feeCap sized per
    /// ceil(amount / MAX_DEPOSIT_RAW) (OI-2's first direction, here 7 fees for
    /// four chunks), a greedy first flusher takes the whole cap on the first
    /// chunk. No later chunk pays a fee, so the remainder waits for a zero-fee
    /// flush per chunk, a refund, or returnToOwner(p, 0) after notAfter.
    function test_OI2_GreedyFirstFlushStrandsTheRemainderAboveMaxDepositRaw() public {
        uint256 delivered = 3 * CAP + 5_000 * UNIT;
        // casting to 'uint96' is safe because the product is 1,750,000
        // forge-lint: disable-next-line(unsafe-typecast)
        uint96 feeCap = uint96(FEE * ((delivered + CAP - 1) / CAP + 3));
        assertEq(uint256(feeCap), 7 * FEE);
        B3ForwarderTypesV2.DepositParams memory p = depositParamsWith(feeCap, NOT_AFTER);
        address account = fd.accountOf(p);
        bridgeFromArbitrum(BOB, account, delivered);
        vm.prank(RELAYER, RELAYER);
        fd.deploy(p);

        assertEq(flushAs(RELAYER2, p, feeCap), CAP);
        uint256 remainder = delivered - CAP - feeCap;
        assertEq(fd.freeBalance(account), remainder);
        expectFlushRevert(RELAYER, p, 1, B3DepositForwarderFactory.FeeCapExceeded.selector);

        // Only unpaid moves remain: a zero-fee flush per chunk until notAfter ...
        uint256 snapshot = vm.snapshotState();
        assertEq(flushAs(ALICE, p, 0), CAP);
        assertEq(flushAs(ALICE, p, 0), CAP);
        assertEq(flushAs(ALICE, p, 0), remainder - 2 * CAP);
        assertTrue(vm.revertToState(snapshot));
        // ... or, after it, returnToOwner(p, 0) of all of it to refundEOA.
        vm.warp(uint256(NOT_AFTER) + 1);
        vm.expectRevert(B3DepositForwarderFactory.FeeCapExceeded.selector);
        vm.prank(RELAYER);
        fd.returnToOwner(p, 1);
        vm.prank(RELAYER);
        assertEq(fd.returnToOwner(p, 0), remainder);
        assertEq(usdt.balanceOf(ALICE), remainder);
        assertEq(fd.freeBalance(account), 0);
    }

    /// The 4-fee cap with cooperative flushers: a delivery above
    /// 3 x MAX_DEPOSIT_RAW, which the wallet never derives a forwarder for,
    /// exhausts the cap after three paid flushes. The remainder stays the
    /// user's: a zero-fee flush until notAfter, or a zero-fee returnToOwner
    /// after it.
    function test_OI2_DefaultFourFeeCapLeavesRemainderForReturnToOwner() public {
        B3ForwarderTypesV2.DepositParams memory p = depositParams();
        assertEq(uint256(p.feeCap), 4 * FEE);
        address account = fd.accountOf(p);
        uint256 delivered = 3 * CAP + 2_000 * UNIT;

        bridgeFromArbitrum(BOB, account, delivered);
        vm.prank(RELAYER, RELAYER);
        fd.deploy(p);
        assertEq(flushAs(RELAYER, p, 2 * FEE), CAP);
        assertEq(flushAs(RELAYER, p, FEE), CAP);
        assertEq(flushAs(RELAYER, p, FEE), CAP);
        assertEq(fd.feesCharged(account), 4 * FEE);
        uint256 remainder = 2_000 * UNIT - 4 * FEE;
        assertEq(fd.freeBalance(account), remainder);
        expectFlushRevert(RELAYER, p, FEE, B3DepositForwarderFactory.FeeCapExceeded.selector);

        // Before notAfter the user may flush the remainder without a fee.
        uint256 snapshot = vm.snapshotState();
        assertEq(flushAs(ALICE, p, 0), remainder);
        assertTrue(vm.revertToState(snapshot));

        // After it, anyone returns the remainder to refundEOA, at no fee.
        vm.warp(uint256(NOT_AFTER) + 1);
        expectFlushRevert(RELAYER, p, 0, B3DepositForwarderFactory.Expired.selector);
        vm.expectRevert(B3DepositForwarderFactory.FeeCapExceeded.selector);
        vm.prank(RELAYER2);
        fd.returnToOwner(p, FEE);
        vm.prank(RELAYER2);
        assertEq(fd.returnToOwner(p, 0), remainder);
        assertEq(usdt.balanceOf(ALICE), remainder);
        assertEq(fd.freeBalance(account), 0);
        assertEq(fd.collectFee(account, RELAYER), 4 * FEE);
        assertEq(usdt.balanceOf(account), 0);
    }

    // =================================================================== D5

    function test_D5_RepeatedMaxFeeFlushesNeverExceedFeeCap() public {
        (B3ForwarderTypesV2.DepositParams memory p, address account) = _deployedFunded(100 * UNIT);
        uint256 feeCap = p.feeCap;

        assertEq(flushAs(RELAYER, p, 3 * FEE), 100 * UNIT - 3 * FEE);
        for (uint256 round = 0; round < 6; ++round) {
            fundUsdt(account, 100 * UNIT);
            uint256 left = feeCap - fd.feesCharged(account);
            // The largest fee the cap still allows is accepted; one more unit is not.
            expectFlushRevert(RELAYER, p, feeCap, B3DepositForwarderFactory.FeeCapExceeded.selector);
            expectFlushRevert(RELAYER, p, left + 1, B3DepositForwarderFactory.FeeCapExceeded.selector);
            expectFlushRevert(RELAYER, p, type(uint256).max, B3DepositForwarderFactory.FeeCapExceeded.selector);
            assertEq(flushAs(RELAYER, p, left), 100 * UNIT - left);
            assertLe(fd.feesCharged(account), feeCap, "D5");
        }
        assertEq(fd.feesCharged(account), feeCap);
        assertEq(fd.reserved(account), feeCap);
        assertEq(vault.locked(), 700 * UNIT - feeCap);
    }

    function test_FeeAboveFeeCapRefusedFromTheStart() public {
        (B3ForwarderTypesV2.DepositParams memory p, address account) = _deployedFunded(100 * UNIT);
        expectFlushRevert(RELAYER, p, uint256(p.feeCap) + 1, B3DepositForwarderFactory.FeeCapExceeded.selector);
        assertEq(flushAs(RELAYER, p, p.feeCap), 100 * UNIT - p.feeCap);
        assertEq(fd.feesCharged(account), p.feeCap);
    }

    // ============================================================= notAfter

    function test_NotAfter_FlushUntilThenExpired() public {
        (B3ForwarderTypesV2.DepositParams memory p, address account) = _deployedFunded(100 * UNIT);
        vm.warp(NOT_AFTER);
        assertEq(flushAs(RELAYER, p, FEE), 100 * UNIT - FEE, "flush at notAfter itself");

        fundUsdt(account, 100 * UNIT);
        vm.warp(uint256(NOT_AFTER) + 1);
        expectFlushRevert(RELAYER, p, FEE, B3DepositForwarderFactory.Expired.selector);
        expectFlushRevert(RELAYER, p, 0, B3DepositForwarderFactory.Expired.selector);
    }

    function test_NotAfter_ReturnToOwnerOnlyAfter() public {
        B3ForwarderTypesV2.DepositParams memory p = depositParams();
        address account = fd.accountOf(p);
        bridgeFromArbitrum(BOB, account, 40 * UNIT);

        vm.warp(NOT_AFTER);
        vm.expectRevert(B3DepositForwarderFactory.NotExpired.selector);
        fd.returnToOwner(p, FEE);

        vm.warp(uint256(NOT_AFTER) + 1);
        vm.expectEmit(true, true, true, true, address(fd));
        emit B3DepositForwarderFactory.ReturnedToOwner(account, ALICE, 40 * UNIT - FEE, FEE);
        vm.prank(RELAYER2);
        assertEq(fd.returnToOwner(p, FEE), 40 * UNIT - FEE);

        assertTrue(fd.isAccount(account), "deployed on demand");
        assertEq(usdt.balanceOf(ALICE), 40 * UNIT - FEE);
        assertEq(fd.owed(account, RELAYER2), FEE);
        assertEq(fd.freeBalance(account), 0);
        vm.expectRevert(B3DepositForwarderFactory.NothingToReturn.selector);
        fd.returnToOwner(p, 0);
        vm.expectRevert(B3DepositForwarderFactory.InsufficientBalance.selector);
        fd.returnToOwner(p, FEE);
    }

    // =============================================================== refund

    function test_Refund_OnlyRefundEOA() public {
        (B3ForwarderTypesV2.DepositParams memory p, address account) = _deployedFunded(10 * UNIT);
        address[3] memory others = [BOB, RELAYER, address(fd)];
        for (uint256 i = 0; i < others.length; ++i) {
            vm.expectRevert(B3DepositForwarderFactory.NotRefundEOA.selector);
            vm.prank(others[i]);
            fd.refund(p, address(usdt), others[i], UNIT);
        }
        vm.expectEmit(true, true, true, true, address(fd));
        emit B3DepositForwarderFactory.Refunded(account, address(usdt), CAROL, UNIT);
        vm.prank(ALICE);
        fd.refund(p, address(usdt), CAROL, UNIT);
        assertEq(usdt.balanceOf(CAROL), UNIT);
    }

    /// D4: a refund never spends the fees a flush accrued.
    function test_Refund_AtMostFree() public {
        (B3ForwarderTypesV2.DepositParams memory p, address account) = _deployedFunded(100 * UNIT);
        flushAs(RELAYER, p, FEE);
        fundUsdt(account, 7 * UNIT);
        assertEq(usdt.balanceOf(account), 7 * UNIT + FEE);
        assertEq(fd.freeBalance(account), 7 * UNIT);

        vm.expectRevert(B3DepositForwarderFactory.InsufficientBalance.selector);
        vm.prank(ALICE);
        fd.refund(p, address(usdt), CAROL, 7 * UNIT + 1);
        vm.prank(ALICE);
        fd.refund(p, address(usdt), CAROL, 7 * UNIT);
        assertEq(usdt.balanceOf(account), FEE);
        assertEq(fd.collectFee(account, RELAYER), FEE);
        assertEq(usdt.balanceOf(account), 0);
    }

    function test_Refund_RefusesVaultAccountFactoryAndZero() public {
        (B3ForwarderTypesV2.DepositParams memory p, address account) = _deployedFunded(10 * UNIT);
        address[4] memory targets = [address(vault), account, address(fd), address(0)];
        for (uint256 i = 0; i < targets.length; ++i) {
            vm.expectRevert(B3DepositForwarderFactory.BadRefundTarget.selector);
            vm.prank(ALICE);
            fd.refund(p, address(usdt), targets[i], UNIT);
        }
        vm.expectRevert(B3DepositForwarderFactory.ZeroAmount.selector);
        vm.prank(ALICE);
        fd.refund(p, address(usdt), CAROL, 0);
        assertEq(usdt.balanceOf(account), 10 * UNIT);
    }

    /// Before deployment (funds credited by the OFT, nobody flushed), the
    /// refund deploys the account on demand; ETH and other tokens leave too.
    function test_Refund_BeforeDeploy() public {
        B3ForwarderTypesV2.DepositParams memory p = depositParams();
        address account = fd.accountOf(p);
        bridgeFromArbitrum(BOB, account, 20 * UNIT);
        vm.deal(account, 1 ether);
        assertEq(account.code.length, 0);

        vm.expectEmit(true, true, true, true, address(fd));
        emit B3DepositForwarderFactory.Published(account, p);
        vm.prank(ALICE);
        fd.refund(p, address(usdt), CAROL, 20 * UNIT);
        assertTrue(fd.isAccount(account));
        assertEq(usdt.balanceOf(CAROL), 20 * UNIT);

        vm.expectRevert(B3DepositForwarderFactory.InsufficientBalance.selector);
        vm.prank(ALICE);
        fd.refund(p, address(0), CAROL, 1 ether + 1);
        vm.prank(ALICE);
        fd.refund(p, address(0), CAROL, 1 ether);
        assertEq(CAROL.balance, 1 ether);
        assertEq(account.balance, 0);

        // A token B3 does not bridge (USDT0 sent by mistake) leaves the same way.
        usdt0.mint(account, 5 * UNIT);
        vm.prank(ALICE);
        fd.refund(p, address(usdt0), CAROL, 5 * UNIT);
        assertEq(usdt0.balanceOf(CAROL), 5 * UNIT);
        vm.expectRevert(abi.encodeWithSelector(B3DepositForwarderFactory.NoCode.selector, address(0xC0DE1E55)));
        vm.prank(ALICE);
        fd.refund(p, address(0xC0DE1E55), CAROL, 1);
    }

    function test_RefundOfForeignTokenCannotReenter() public {
        (B3ForwarderTypesV2.DepositParams memory p,) = _deployedFunded(10 * UNIT);
        ReenteringTokenFdV2 evil = new ReenteringTokenFdV2(fd);
        vm.expectRevert(B3DepositForwarderFactory.Reentrancy.selector);
        vm.prank(ALICE);
        fd.refund(p, address(evil), CAROL, 1);
    }

    // ===================================================== deposits closed

    /// The vault's DepositClosed bubbles out of the flush and undoes the
    /// accrual; the refund keeps working (spec §10 escapes).
    function test_DepositsClosed_FlushRevertsAndRefundWorks() public {
        (B3ForwarderTypesV2.DepositParams memory p, address account) = _deployedFunded(100 * UNIT);
        verifier.setDepositsOpen(false);
        expectFlushRevert(RELAYER, p, FEE, B3VaultV2.DepositClosed.selector);
        expectFlushRevert(RELAYER, p, 0, B3VaultV2.DepositClosed.selector);
        assertEq(fd.feesCharged(account), 0);
        assertEq(fd.reserved(account), 0);
        assertEq(fd.owed(account, RELAYER), 0);
        assertEq(vault.nextDepositId(), 0);

        vm.prank(ALICE);
        fd.refund(p, address(usdt), CAROL, 100 * UNIT);
        assertEq(usdt.balanceOf(CAROL), 100 * UNIT);
    }

    /// A Tether pause closes the vault's gate too; the funds wait (a paused
    /// token moves nothing) and leave by refund after the unpause.
    function test_TetherPaused_FlushRefusedFundsWait() public {
        (B3ForwarderTypesV2.DepositParams memory p, address account) = _deployedFunded(100 * UNIT);
        usdt.pause();
        expectFlushRevert(RELAYER, p, FEE, B3VaultV2.DepositClosed.selector);
        vm.expectRevert();
        vm.prank(ALICE);
        fd.refund(p, address(usdt), CAROL, 100 * UNIT);
        usdt.unpause();
        assertEq(usdt.balanceOf(account), 100 * UNIT);
        vm.prank(ALICE);
        fd.refund(p, address(usdt), CAROL, 100 * UNIT);
        assertEq(usdt.balanceOf(CAROL), 100 * UNIT);
    }

    /// Tether blacklisting F_d stops the flush (the vault's transferFrom
    /// fails), the refund and returnToOwner (its transfer fails).
    function test_BlacklistedAccount_FlushAndRefundRevert() public {
        (B3ForwarderTypesV2.DepositParams memory p, address account) = _deployedFunded(100 * UNIT);
        usdt.addBlackList(account);
        expectFlushRevert(RELAYER, p, FEE, B3VaultV2.TransferFailed.selector);
        vm.expectRevert();
        vm.prank(ALICE);
        fd.refund(p, address(usdt), CAROL, UNIT);
        vm.warp(uint256(NOT_AFTER) + 1);
        vm.expectRevert();
        fd.returnToOwner(p, 0);
        assertEq(usdt.balanceOf(account), 100 * UNIT);
        assertEq(fd.feesCharged(account), 0);

        usdt.removeBlackList(account);
        vm.prank(ALICE);
        fd.refund(p, address(usdt), CAROL, 100 * UNIT);
        assertEq(usdt.balanceOf(CAROL), 100 * UNIT);
    }

    // ========================================================== collectFee

    function test_CollectFee_PaysExactlyTheAccrued() public {
        (B3ForwarderTypesV2.DepositParams memory p, address account) = _deployedFunded(25_000 * UNIT);
        flushAs(RELAYER, p, FEE);
        flushAs(RELAYER2, p, 2 * FEE);
        flushAs(RELAYER, p, FEE / 2);
        assertEq(fd.owed(account, RELAYER), FEE + FEE / 2);
        assertEq(fd.owed(account, RELAYER2), 2 * FEE);
        assertEq(fd.reserved(account), 3 * FEE + FEE / 2);

        vm.expectEmit(true, true, true, true, address(fd));
        emit B3DepositForwarderFactory.FeeCollected(account, RELAYER, FEE + FEE / 2);
        vm.prank(CAROL); // anyone may trigger it; the payee is paid
        assertEq(fd.collectFee(account, RELAYER), FEE + FEE / 2);
        assertEq(usdt.balanceOf(RELAYER), FEE + FEE / 2);
        assertEq(fd.reserved(account), 2 * FEE);
        vm.expectRevert(B3DepositForwarderFactory.NothingOwed.selector);
        fd.collectFee(account, RELAYER);
        vm.expectRevert(B3DepositForwarderFactory.NothingOwed.selector);
        fd.collectFee(account, CAROL);

        assertEq(fd.collectFee(account, RELAYER2), 2 * FEE);
        assertEq(usdt.balanceOf(RELAYER2), 2 * FEE);
        assertEq(fd.reserved(account), 0);
        assertEq(usdt.balanceOf(account), 0);
        assertEq(fd.feesCharged(account), 3 * FEE + FEE / 2, "the lifetime total stays");
    }

    // ================================================================= fuzz

    struct Model {
        uint256 arrived;
        uint256 deposited;
        uint256 refunded;
        uint256 collected;
        uint256 feesCharged;
        uint256 owedA; // RELAYER
        uint256 owedB; // RELAYER2
        bool deployed;
    }

    /// Random sequences of arrivals, deploys, flushes, refunds and fee
    /// collections. Every call's outcome is predicted exactly, and after
    /// every step: D2 (tokens left only to the vault, payees and refundEOA's
    /// target), D3 (balance >= reserved == owed), D4 (free = balance -
    /// reserved, refunds never touch reserved), D5 (feesCharged <= feeCap)
    /// and D6 (each flush deposits min(free - fee, MAX)).
    function testFuzz_DepositInvariantsD2ToD6(uint256 seed) public {
        // casting to 'uint96' is safe because 12 * FEE is 3,000,000
        // forge-lint: disable-next-line(unsafe-typecast)
        B3ForwarderTypesV2.DepositParams memory p = depositParamsWith(uint96(12 * FEE), NOT_AFTER);
        address account = fd.accountOf(p);
        Model memory m;
        for (uint256 i = 0; i < 24; ++i) {
            uint256 r = uint256(keccak256(abi.encode(seed, i)));
            uint256 op = r % 8;
            uint256 x = r >> 8;
            if (op <= 1) {
                uint256 amount = (x % 3 == 0) ? (x >> 4) % (2 * UNIT) + 1 : (x >> 4) % (25_000 * UNIT) + 1;
                fundUsdt(account, amount);
                m.arrived += amount;
            } else if (op <= 4) {
                _fuzzFlush(p, account, m, x);
            } else if (op == 5) {
                _fuzzRefund(p, account, m, x);
            } else if (op == 6) {
                _fuzzCollect(account, m, x);
            } else {
                vm.prank(RELAYER);
                try fd.deploy(p) {
                    assertFalse(m.deployed);
                    m.deployed = true;
                } catch (bytes memory reason) {
                    assertTrue(m.deployed);
                    _assertError(reason, B3DepositForwarderFactory.AlreadyDeployed.selector, "deploy error");
                }
            }
            _checkInvariants(p, account, m);
        }
    }

    function _fuzzFlush(B3ForwarderTypesV2.DepositParams memory p, address account, Model memory m, uint256 x) private {
        address payee = (x & 1) == 0 ? RELAYER : RELAYER2;
        uint256 fee = (x >> 1) % (3 * FEE);
        uint256 free = usdt.balanceOf(account) - (m.owedA + m.owedB);
        uint256 expectedAmount = 0;
        bytes4 expected;
        if (!m.deployed) {
            expected = B3DepositForwarderFactory.NotDeployed.selector;
        } else if (fee > p.feeCap || m.feesCharged > p.feeCap - fee) {
            expected = B3DepositForwarderFactory.FeeCapExceeded.selector;
        } else if (fee > free) {
            expected = B3DepositForwarderFactory.InsufficientBalance.selector;
        } else {
            expectedAmount = free - fee < CAP ? free - fee : CAP;
            if (expectedAmount < MIN_FLUSH) expected = B3DepositForwarderFactory.BelowMinimum.selector;
        }

        uint256 vaultBefore = usdt.balanceOf(address(vault));
        vm.prank(payee, payee);
        try fd.flush(p, fee) returns (uint256 amount) {
            assertEq(uint256(uint32(expected)), 0, "flush expected to fail");
            assertEq(amount, expectedAmount, "D6");
            assertEq(usdt.balanceOf(address(vault)) - vaultBefore, amount, "D6: the vault received it");
            m.deposited += amount;
            m.feesCharged += fee;
            if (payee == RELAYER) m.owedA += fee;
            else m.owedB += fee;
        } catch (bytes memory reason) {
            _assertError(reason, expected, "flush error");
        }
    }

    function _fuzzRefund(B3ForwarderTypesV2.DepositParams memory p, address account, Model memory m, uint256 x)
        private
    {
        uint256 free = usdt.balanceOf(account) - (m.owedA + m.owedB);
        uint256 amount = x % (free + 2);
        bytes4 expected;
        if (amount == 0) expected = B3DepositForwarderFactory.ZeroAmount.selector;
        else if (amount > free) expected = B3DepositForwarderFactory.InsufficientBalance.selector;

        vm.prank(ALICE);
        try fd.refund(p, address(usdt), CAROL, amount) {
            assertEq(uint256(uint32(expected)), 0, "refund expected to fail");
            m.refunded += amount;
            m.deployed = true;
        } catch (bytes memory reason) {
            _assertError(reason, expected, "refund error");
        }
    }

    function _fuzzCollect(address account, Model memory m, uint256 x) private {
        bool first = (x & 1) == 0;
        address payee = first ? RELAYER : RELAYER2;
        uint256 owed = first ? m.owedA : m.owedB;
        try fd.collectFee(account, payee) returns (uint256 amount) {
            assertEq(amount, owed);
            assertGt(owed, 0);
            m.collected += amount;
            if (first) m.owedA = 0;
            else m.owedB = 0;
        } catch (bytes memory reason) {
            assertEq(owed, 0);
            _assertError(reason, B3DepositForwarderFactory.NothingOwed.selector, "collect error");
        }
    }

    function _checkInvariants(B3ForwarderTypesV2.DepositParams memory p, address account, Model memory m) private view {
        uint256 balance = usdt.balanceOf(account);
        uint256 reserved = fd.reserved(account);
        // D2: everything that arrived is in the account, the vault, a payee or the refund target.
        assertEq(balance, m.arrived - m.deposited - m.collected - m.refunded, "D2: account");
        assertEq(usdt.balanceOf(address(vault)), m.deposited, "D2: vault");
        assertEq(vault.locked(), m.deposited);
        assertEq(usdt.balanceOf(CAROL), m.refunded, "D2: refund target");
        assertEq(usdt.balanceOf(RELAYER) + usdt.balanceOf(RELAYER2), m.collected, "D2: payees");
        // D3 and D4.
        assertEq(reserved, m.owedA + m.owedB, "D3: reserved is what payees are owed");
        assertEq(fd.owed(account, RELAYER), m.owedA);
        assertEq(fd.owed(account, RELAYER2), m.owedB);
        assertGe(balance, reserved);
        assertEq(fd.freeBalance(account), balance - reserved, "D4");
        // D5.
        assertEq(fd.feesCharged(account), m.feesCharged);
        assertLe(m.feesCharged, p.feeCap, "D5");
        assertEq(fd.isAccount(account), m.deployed);
    }

    // ============================================================= runtime

    function test_RuntimeSizesWithinEip170() public {
        (, address account) = _deployedFunded(0);
        assertSizes(
            "B3DepositForwarderFactory", address(fd).code.length, type(B3DepositForwarderFactory).creationCode.length
        );
        assertSizes("B3Forwarder", account.code.length, type(B3Forwarder).creationCode.length);
        ConsoleV2.log("B3DepositForwarderFactory runtime bytes", address(fd).code.length);
        ConsoleV2.log("B3Forwarder runtime bytes", account.code.length);
    }

    /// Spec §12: no NUMBER in the factory or B3Forwarder (both may later run
    /// on Arbitrum). The factory writes storage and transient storage (its
    /// lock) and deploys accounts with CREATE2 only.
    function test_RuntimesHaveNoNumberOpcode() public {
        (, address account) = _deployedFunded(0);
        bytes memory code = address(fd).code;
        assertFactoryRuntimeClean("B3DepositForwarderFactory", code);
        (uint256[256] memory counts,) = factoryOpcodeCounts(code);
        assertGt(counts[Scan.SSTORE], 0);
        assertGt(counts[Scan.TSTORE], 0);
        assertForwarderRuntimeClean(account);
    }

    /// Only the factory drives an account; it takes no plain ETH.
    function test_ForwarderExecOnlyByFactory() public {
        (, address account) = _deployedFunded(10 * UNIT);
        vm.expectRevert(B3Forwarder.NotFactory.selector);
        vm.prank(ALICE);
        B3Forwarder(account).exec(address(usdt), 0, abi.encodeWithSignature("transfer(address,uint256)", ALICE, 1));
        vm.deal(ALICE, 1 ether);
        vm.prank(ALICE);
        (bool ok,) = account.call{value: 1}("");
        assertFalse(ok, "no receive");
    }

    // ============================================================= helpers

    /// A deployed default F_d holding `amount` USDT.
    function _deployedFunded(uint256 amount)
        private
        returns (B3ForwarderTypesV2.DepositParams memory p, address account)
    {
        p = depositParams();
        vm.prank(RELAYER, RELAYER);
        account = fd.deploy(p);
        if (amount != 0) fundUsdt(account, amount);
    }

    function _vault(address forwarderFactory, uint256 maxDepositRaw) private returns (B3VaultV2) {
        return new B3VaultV2(
            B3VaultV2.Config({
                verifier: IB3FinalityVerifierV2(address(verifier)),
                verifierCodeHash: address(verifier).codehash,
                b3ChainDomain: DOMAIN,
                originToken: address(usdt),
                tokenCodeHash: address(usdt).codehash,
                forwarderFactory: forwarderFactory,
                maxDepositRaw: maxDepositRaw,
                predecessor: address(0),
                predecessorAssetId: bytes32(0)
            })
        );
    }

    function _assertError(bytes memory reason, bytes4 expected, string memory err) private pure {
        // casting is safe: only the selector is compared
        // forge-lint: disable-next-line(unsafe-typecast)
        assertEq(bytes32(bytes4(reason)), bytes32(expected), err);
    }
}
