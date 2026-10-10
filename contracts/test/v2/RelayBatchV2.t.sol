// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {VmV2, ConsoleV2} from "./V2TestBase.sol";
import {RelayFixtureV2} from "./utils/RelayFixtureV2.sol";
import {BlsTestSignerV2 as Signer} from "./utils/BlsTestSignerV2.sol";
import {MerkleTreeV2} from "./utils/MerkleTreeV2.sol";
import {RuntimeScanV2 as Scan} from "./utils/RuntimeScanV2.sol";
import {B3TypesV2} from "../../src/v2/B3TypesV2.sol";
import {B3FinalityVerifierV2} from "../../src/v2/B3FinalityVerifierV2.sol";
import {B3VaultV2} from "../../src/v2/B3VaultV2.sol";
import {B3RelayBatch} from "../../src/v2/B3RelayBatch.sol";
import {BlsDigestProver} from "../../src/v2/BlsDigestProver.sol";
import {IB3FinalityVerifierV2} from "../../src/v2/IB3FinalityVerifierV2.sol";

/// TEST ONLY. A vault stand-in whose released() answers oddly and whose
/// release() only counts calls: the batch must skip an item only on a clean
/// 32-byte `true`, and try (and here pay) it on any other answer.
contract OddAnswerVaultV2 {
    enum Answer {
        RevertWithOne,
        SixtyFourBytesStartingWithOne,
        Two,
        True
    }

    Answer public answer;
    uint256 public releaseCalls;

    function setAnswer(Answer newAnswer) external {
        answer = newAnswer;
    }

    function released(uint64) external view returns (bool) {
        Answer a = answer;
        assembly {
            mstore(0x00, 1)
            mstore(0x20, 0)
            switch a
            case 0 { revert(0x00, 0x20) }
            case 1 { return(0x00, 0x40) }
            case 2 {
                mstore(0x00, 2)
                return(0x00, 0x20)
            }
            default { return(0x00, 0x20) }
        }
    }

    function release(B3TypesV2.WithdrawalV2 calldata, bytes32[32] calldata) external {
        ++releaseCalls;
    }
}

/// B3RelayBatch (contracts spec §9) on the real V2 stack: per-item failure
/// isolation for certificates and releases, skipped released ids, codeless
/// and wrong targets, no ETH entry and no balance, a stateless runtime, and
/// a 64-member transition certificate plus a release within the EIP-7825 cap.
///
/// setUp: a K_OF_N verifier bootstrapped at checkpoint 1, Set'1 takes over at
/// V (checkpoint 10) with the empty withdrawal tree, deposits open and ALICE
/// deposits 1,000 USDT, so the vault holds a reserve for every release below.
contract RelayBatchV2Test is RelayFixtureV2 {
    uint256 private constant EIP7825_TX_GAS_CAP = 16_777_216;

    B3FinalityVerifierV2 private v;
    B3VaultV2 private vault;

    function setUp() public override {
        super.setUp();
        (v, vault) = deployBridge();
        bootstrap(v, 1, lineageSet(1));
        vm.warp(T0 + REVIEW);
        submit(v, canonicalCert(V_INDEX, 1, lineageSet(1), lineageSet(2), emptyTreeRoot()));
        assertTrue(vault.depositGateOpen());
        depositAs(vault, ALICE, 1_000 * UNIT);
        assertEq(vault.locked(), 1_000 * UNIT);
    }

    // ========================================================== isolation

    /// The acceptance case: a failing middle certificate, an already released
    /// item, an invalid item and a duplicate each fail or skip alone; every
    /// other certificate and release in the same batch still applies, in order.
    function test_BatchIsolatesAFailedCertificateAndFailedOrSkippedReleases() public {
        Signer.TestSet memory s1 = lineageSet(1);
        Signer.TestSet memory s2 = lineageSet(2);
        B3TypesV2.WithdrawalV2[] memory ws = new B3TypesV2.WithdrawalV2[](4);
        ws[0] = withdrawal(0, BOB, 100 * UNIT, V + 1);
        ws[1] = withdrawal(1, CAROL, 200 * UNIT, V + 2);
        ws[2] = withdrawal(2, DAVE, 300 * UNIT, heightAt(12) - 3);
        ws[3] = withdrawal(3, BOB, 50 * UNIT, heightAt(12) + 2);
        bytes32[] memory leaves = treeOf(vault, ws);

        // Before the batch: leaves 0-1 certified at checkpoint 11; id 1 released directly.
        submit(v, canonicalCert(11, V_INDEX, s1, s2, rootOf(leaves, 2)));
        vault.release(ws[1], MerkleTreeV2.path32(leaves, 2, 1));

        // Certificates: 12 is valid (leaves 0-2); 13 carries only 2 of 4
        // signatures (headcount quorum is 3); 14 extends 12 (leaves 0-3).
        Cert memory c12 = canonicalCert(12, 11, s1, s2, rootOf(leaves, 3));
        Cert memory c13 = certWithRoot(CANONICAL, 13, 12, s1, s2, rootOf(leaves, 3), 0, Signer.ids(2, 3));
        Cert memory c14 = canonicalCert(14, 12, s1, s2, rootOf(leaves, 4));

        B3RelayBatch.ReleaseItem[] memory rel = new B3RelayBatch.ReleaseItem[](6);
        rel[0] = item(leaves, 3, ws[0]); // root of 12: pays
        rel[1] = item(leaves, 3, ws[1]); // released before: skipped
        rel[2] = item(leaves, 3, ws[2]);
        rel[2].w.amount += 1; // not the certified leaf: UnknownRoot
        rel[3] = item(leaves, 4, ws[3]); // root of 14, accepted after the failed 13: pays
        rel[4] = item(leaves, 3, ws[0]); // released by rel[0]: skipped
        rel[5] = item(leaves, 4, ws[2]); // the real leaf 2 under 14's root: pays

        vm.recordLogs();
        (uint256 certsAccepted, uint256 releasesPaid) = relay(v, inputs(c12, c13, c14), address(vault), rel);
        VmV2.Log[] memory logs = logsOf(vm.getRecordedLogs(), address(batch));

        assertEq(certsAccepted, 2);
        assertEq(releasesPaid, 3);
        assertEq(logs.length, 4, "exactly the failed and skipped items");
        assertCertificateFailed(logs[0], 1, selectorError(B3FinalityVerifierV2.ProofRejected.selector));
        assertReleaseSkipped(logs[1], 1, 1);
        assertReleaseFailed(logs[2], 2, 2, selectorError(B3VaultV2.UnknownRoot.selector));
        assertReleaseSkipped(logs[3], 4, 0);

        assertEq(uint256(v.latestIndex()), 14);
        assertEq(uint256(v.acceptedRootHeight(rootOf(leaves, 3))), heightAt(12));
        assertEq(uint256(v.acceptedRootHeight(rootOf(leaves, 4))), heightAt(14));
        assertEq(usdt.balanceOf(BOB), 150 * UNIT);
        assertEq(usdt.balanceOf(CAROL), 200 * UNIT);
        assertEq(usdt.balanceOf(DAVE), 300 * UNIT);
        assertEq(vault.locked(), 350 * UNIT);
        for (uint64 id = 0; id < 4; ++id) {
            assertTrue(vault.released(id));
        }
    }

    /// A malformed BLS witness burns all gas forwarded to the failing EIP-2537
    /// precompile. The absent-member witness here keeps the member's x and
    /// sign bit, so it passes the compressed-key match and the member path,
    /// but its y is off the curve, so G1ADD fails. With the per-item cap the
    /// bad certificate costs at most CERT_GAS and the valid certificate and
    /// release after it in the same batch still apply. Forwarding all gas
    /// instead (type(uint256).max), the same batch in the same budget loses
    /// the valid certificate.
    function test_GasCapContainsAPrecompileGasBurn() public {
        Signer.TestSet memory s1 = lineageSet(1);
        Signer.TestSet memory s2 = lineageSet(2);
        B3TypesV2.WithdrawalV2[] memory ws = new B3TypesV2.WithdrawalV2[](1);
        ws[0] = withdrawal(0, BOB, 100 * UNIT, V + 1);
        bytes32[] memory leaves = treeOf(vault, ws);
        Cert memory good = canonicalCert(11, V_INDEX, s1, s2, rootOf(leaves, 1));
        Cert memory bad = canonicalCert(11, V_INDEX, s1, s2, rootOf(leaves, 1));
        bad.proof = _offCurveAbsentWitness(bad.proof);
        B3RelayBatch.CertInput[] memory certs = new B3RelayBatch.CertInput[](2);
        certs[0] = toInput(bad);
        certs[1] = toInput(good);
        B3RelayBatch.ReleaseItem[] memory rel = items(item(leaves, 1, ws[0]));
        uint256 budget = 12_000_000;

        // The bad certificate alone, capped: it burns at most its cap.
        uint256 snapshot = vm.snapshotState();
        B3RelayBatch.CertInput[] memory badOnly = new B3RelayBatch.CertInput[](1);
        badOnly[0] = certs[0];
        uint256 g0 = gasleft();
        (bool ok, bytes memory ret) = address(batch).call{gas: budget}(
            abi.encodeCall(B3RelayBatch.relay, (v, badOnly, CERT_GAS, address(vault), noReleases(), RELEASE_GAS))
        );
        uint256 badCost = g0 - gasleft();
        assertTrue(ok, "capped batch with the bad certificate");
        (uint256 certsAccepted,) = abi.decode(ret, (uint256, uint256));
        assertEq(certsAccepted, 0);
        assertGt(badCost, CERT_GAS * 9 / 10); // the precompile burned most of the cap
        assertLt(badCost, CERT_GAS + 200_000, "and no more than the cap");
        ConsoleV2.log("batch gas, one off-curve certificate under CERT_GAS", badCost);
        assertTrue(vm.revertToState(snapshot));

        // Uncapped: the bad item leaves too little gas for the valid certificate.
        snapshot = vm.snapshotState();
        (ok, ret) = address(batch).call{gas: budget}(
            abi.encodeCall(B3RelayBatch.relay, (v, certs, type(uint256).max, address(vault), rel, type(uint256).max))
        );
        if (ok) {
            (certsAccepted,) = abi.decode(ret, (uint256, uint256));
            assertEq(certsAccepted, 0, "uncapped: the valid certificate starved");
        }
        ConsoleV2.log(ok ? "uncapped batch: returned, 0 certificates accepted" : "uncapped batch: ran out of gas");
        assertEq(uint256(v.latestIndex()), V_INDEX);
        assertTrue(vm.revertToState(snapshot));

        // Capped, same budget: the bad item fails alone; the rest applies.
        vm.recordLogs();
        (ok, ret) = address(batch).call{gas: budget}(
            abi.encodeCall(B3RelayBatch.relay, (v, certs, CERT_GAS, address(vault), rel, RELEASE_GAS))
        );
        VmV2.Log[] memory logs = logsOf(vm.getRecordedLogs(), address(batch));
        assertTrue(ok, "capped batch");
        uint256 releasesPaid;
        (certsAccepted, releasesPaid) = abi.decode(ret, (uint256, uint256));
        assertEq(certsAccepted, 1);
        assertEq(releasesPaid, 1);
        assertEq(logs.length, 1);
        assertCertificateFailed(logs[0], 0, abi.encodeWithSignature("Error(string)", "BLS_G1_ADD"));
        assertEq(uint256(v.latestIndex()), 11);
        assertEq(usdt.balanceOf(BOB), 100 * UNIT);
        assertBatchEmpty();
    }

    /// Root history: a batch without certificates pays one leaf under an
    /// older accepted root and another under a newer one. The verifier
    /// argument is unused (here codeless) when no certificate is given.
    function test_ReleasesUnderOlderAndNewerRootsWithoutCertificates() public {
        Signer.TestSet memory s1 = lineageSet(1);
        Signer.TestSet memory s2 = lineageSet(2);
        B3TypesV2.WithdrawalV2[] memory ws = new B3TypesV2.WithdrawalV2[](2);
        ws[0] = withdrawal(0, BOB, 100 * UNIT, V + 1);
        ws[1] = withdrawal(1, CAROL, 200 * UNIT, heightAt(11) + 1);
        bytes32[] memory leaves = treeOf(vault, ws);
        submit(v, canonicalCert(11, V_INDEX, s1, s2, rootOf(leaves, 1)));
        submit(v, canonicalCert(12, 11, s1, s2, rootOf(leaves, 2)));

        vm.startPrank(DAVE, DAVE); // anyone may relay
        (uint256 certsAccepted, uint256 releasesPaid) = batch.relay(
            IB3FinalityVerifierV2(address(0xC0DE1E55)),
            noCerts(),
            CERT_GAS,
            address(vault),
            items(item(leaves, 1, ws[0]), item(leaves, 2, ws[1])),
            RELEASE_GAS
        );
        vm.stopPrank();
        assertBatchEmpty();
        assertEq(certsAccepted, 0);
        assertEq(releasesPaid, 2);
        assertEq(usdt.balanceOf(BOB), 100 * UNIT);
        assertEq(usdt.balanceOf(CAROL), 200 * UNIT);
    }

    /// Tether paused: the release reverts whole inside the batch (reported),
    /// the leaf stays unreleased and the reserve unchanged, while a
    /// certificate in the same batch is accepted; after unpause it pays once.
    function test_TransferFailureIsReportedAndTheLeafStaysPayable() public {
        Signer.TestSet memory s1 = lineageSet(1);
        Signer.TestSet memory s2 = lineageSet(2);
        B3TypesV2.WithdrawalV2[] memory ws = new B3TypesV2.WithdrawalV2[](1);
        ws[0] = withdrawal(0, BOB, 400 * UNIT, V + 1);
        bytes32[] memory leaves = treeOf(vault, ws);
        Cert memory c11 = canonicalCert(11, V_INDEX, s1, s2, rootOf(leaves, 1));
        B3RelayBatch.ReleaseItem[] memory rel = items(item(leaves, 1, ws[0]));

        usdt.pause();
        vm.recordLogs();
        (uint256 certsAccepted, uint256 releasesPaid) = relay(v, inputs(c11), address(vault), rel);
        VmV2.Log[] memory logs = logsOf(vm.getRecordedLogs(), address(batch));
        assertEq(certsAccepted, 1);
        assertEq(releasesPaid, 0);
        assertEq(logs.length, 1);
        assertReleaseFailed(logs[0], 0, 0, selectorError(B3VaultV2.TransferFailed.selector));
        assertFalse(vault.released(0));
        assertEq(vault.locked(), 1_000 * UNIT);
        assertEq(usdt.balanceOf(BOB), 0);

        usdt.unpause();
        (certsAccepted, releasesPaid) = relay(v, noCerts(), address(vault), rel);
        assertEq(releasesPaid, 1);
        assertEq(usdt.balanceOf(BOB), 400 * UNIT);
        assertEq(vault.locked(), 600 * UNIT);

        // Once paid, the same item is skipped.
        vm.recordLogs();
        (, releasesPaid) = relay(v, noCerts(), address(vault), rel);
        logs = logsOf(vm.getRecordedLogs(), address(batch));
        assertEq(releasesPaid, 0);
        assertEq(logs.length, 1);
        assertReleaseSkipped(logs[0], 0, 0);
        assertEq(usdt.balanceOf(BOB), 400 * UNIT);
    }

    /// A target with code but the wrong interface fails only its own items:
    /// the token named as vault (its released() reverts, so the item is tried
    /// and reported), and the vault named as verifier.
    function test_WrongTargetsFailOnlyTheirOwnItems() public {
        Signer.TestSet memory s1 = lineageSet(1);
        Signer.TestSet memory s2 = lineageSet(2);
        B3TypesV2.WithdrawalV2[] memory ws = new B3TypesV2.WithdrawalV2[](1);
        ws[0] = withdrawal(0, BOB, 100 * UNIT, V + 1);
        bytes32[] memory leaves = treeOf(vault, ws);
        Cert memory c11 = canonicalCert(11, V_INDEX, s1, s2, rootOf(leaves, 1));
        B3RelayBatch.ReleaseItem[] memory rel = items(item(leaves, 1, ws[0]));

        vm.recordLogs();
        (uint256 certsAccepted, uint256 releasesPaid) = relay(v, inputs(c11), address(usdt), rel);
        VmV2.Log[] memory logs = logsOf(vm.getRecordedLogs(), address(batch));
        assertEq(certsAccepted, 1);
        assertEq(releasesPaid, 0);
        assertEq(logs.length, 1);
        assertReleaseFailed(logs[0], 0, 0, "");
        assertEq(uint256(v.latestIndex()), 11);

        Cert memory c12 = canonicalCert(12, 11, s1, s2, rootOf(leaves, 1));
        vm.recordLogs();
        (certsAccepted, releasesPaid) = relay(IB3FinalityVerifierV2(address(vault)), inputs(c12), address(vault), rel);
        logs = logsOf(vm.getRecordedLogs(), address(batch));
        assertEq(certsAccepted, 0);
        assertEq(releasesPaid, 1);
        assertEq(logs.length, 1);
        assertCertificateFailed(logs[0], 0, "");
        assertEq(uint256(v.latestIndex()), 11);
        assertEq(usdt.balanceOf(BOB), 100 * UNIT);
    }

    /// Only a successful, exactly 32-byte answer equal to 1 skips an item: a
    /// released() that reverts with the word 1, answers 64 bytes or answers 2
    /// leaves the item to be tried.
    function test_SkipOnlyOnACleanTrueFromReleased() public {
        OddAnswerVaultV2 odd = new OddAnswerVaultV2();
        B3RelayBatch.ReleaseItem[] memory rel = new B3RelayBatch.ReleaseItem[](1);
        rel[0].w = withdrawal(7, BOB, 1, V);

        OddAnswerVaultV2.Answer[3] memory tried = [
            OddAnswerVaultV2.Answer.RevertWithOne,
            OddAnswerVaultV2.Answer.SixtyFourBytesStartingWithOne,
            OddAnswerVaultV2.Answer.Two
        ];
        for (uint256 k = 0; k < tried.length; ++k) {
            odd.setAnswer(tried[k]);
            vm.recordLogs();
            (, uint256 releasesPaid) = relay(v, noCerts(), address(odd), rel);
            assertEq(releasesPaid, 1, "tried");
            assertEq(odd.releaseCalls(), k + 1);
            assertEq(logsOf(vm.getRecordedLogs(), address(batch)).length, 0);
        }

        odd.setAnswer(OddAnswerVaultV2.Answer.True);
        vm.recordLogs();
        (, uint256 paid) = relay(v, noCerts(), address(odd), rel);
        VmV2.Log[] memory logs = logsOf(vm.getRecordedLogs(), address(batch));
        assertEq(paid, 0);
        assertEq(odd.releaseCalls(), 3, "skipped, not called");
        assertEq(logs.length, 1);
        assertReleaseSkipped(logs[0], 0, 7);
    }

    /// try/catch cannot catch a call to a codeless address, so a codeless
    /// target for a non-empty list reverts up front; empty lists ignore it.
    function test_CodelessTargetsRevertUpFrontAndEmptyBatchIsANoOp() public {
        address codeless = address(0xC0DE1E55);
        Cert memory c11 = canonicalCert(11, V_INDEX, lineageSet(1), lineageSet(2), emptyTreeRoot());
        B3TypesV2.WithdrawalV2[] memory ws = new B3TypesV2.WithdrawalV2[](1);
        ws[0] = withdrawal(0, BOB, 100 * UNIT, V + 1);
        bytes32[] memory leaves = treeOf(vault, ws);
        B3RelayBatch.ReleaseItem[] memory rel = items(item(leaves, 1, ws[0]));

        B3RelayBatch.CertInput[] memory certs = inputs(c11);
        vm.expectRevert(abi.encodeWithSelector(B3RelayBatch.NoCode.selector, codeless));
        batch.relay(IB3FinalityVerifierV2(codeless), certs, CERT_GAS, address(vault), rel, RELEASE_GAS);

        vm.expectRevert(abi.encodeWithSelector(B3RelayBatch.NoCode.selector, codeless));
        batch.relay(v, certs, CERT_GAS, codeless, rel, RELEASE_GAS);

        // A codeless vault reverts the whole batch, its valid certificate included.
        assertEq(uint256(v.latestIndex()), V_INDEX);

        vm.recordLogs();
        (uint256 certsAccepted, uint256 releasesPaid) =
            relay(IB3FinalityVerifierV2(codeless), noCerts(), codeless, noReleases());
        assertEq(certsAccepted, 0);
        assertEq(releasesPaid, 0);
        assertEq(vm.getRecordedLogs().length, 0);
    }

    /// relay is not payable; relayWithForwards is, and returns every wei it
    /// does not spend (RelayBatchExitV2.t.sol). No receive or fallback: the
    /// batch takes ETH only as that budget. The vault pays recipients
    /// directly, never the batch.
    function test_RefusesEthAndHoldsNothing() public {
        vm.deal(address(this), 1 ether);
        bytes memory call =
            abi.encodeCall(B3RelayBatch.relay, (v, noCerts(), CERT_GAS, address(vault), noReleases(), RELEASE_GAS));
        (bool ok,) = address(batch).call{value: 1}(call);
        assertFalse(ok, "relay is not payable");
        (ok,) = address(batch).call{value: 1}("");
        assertFalse(ok, "no receive");
        (ok,) = address(batch).call{value: 1}(hex"deadbeef");
        assertFalse(ok, "no fallback");
        (ok,) = address(batch).call(call);
        assertTrue(ok, "the same call without value");
        assertBatchEmpty();
    }

    /// Stateless: the runtime has no storage, transient storage, call-code,
    /// creation or self-destruct opcode, and no NUMBER (contracts spec §12's
    /// rule for V2 runtimes); no slot is ever written by a full batch.
    function test_RuntimeIsStatelessAndNumberFree() public {
        Signer.TestSet memory s1 = lineageSet(1);
        Signer.TestSet memory s2 = lineageSet(2);
        B3TypesV2.WithdrawalV2[] memory ws = new B3TypesV2.WithdrawalV2[](1);
        ws[0] = withdrawal(0, BOB, 100 * UNIT, V + 1);
        bytes32[] memory leaves = treeOf(vault, ws);
        relay(
            v,
            inputs(canonicalCert(11, V_INDEX, s1, s2, rootOf(leaves, 1))),
            address(vault),
            items(item(leaves, 1, ws[0]))
        );
        for (uint256 slot = 0; slot < 8; ++slot) {
            assertEq(vm.load(address(batch), bytes32(slot)), bytes32(0));
        }

        bytes memory code = address(batch).code;
        (uint256[256] memory counts, uint256 opcodes) = Scan.opcodeCounts(code);
        assertEq(counts[Scan.SLOAD] + counts[Scan.SSTORE], 0, "no SLOAD/SSTORE");
        assertEq(counts[Scan.TLOAD] + counts[Scan.TSTORE], 0, "no TLOAD/TSTORE");
        assertEq(counts[Scan.CREATE] + counts[Scan.CREATE2], 0, "no CREATE/CREATE2");
        assertEq(counts[Scan.CALLCODE] + counts[Scan.DELEGATECALL], 0, "no CALLCODE/DELEGATECALL");
        assertEq(counts[Scan.SELFDESTRUCT], 0, "no SELFDESTRUCT");
        assertEq(counts[Scan.NUMBER], 0, "no NUMBER");
        assertGt(opcodes, 100);
        ConsoleV2.log("B3RelayBatch runtime bytes", code.length);
        assertLe(code.length, 24_576, "EIP-170");
    }

    // ================================================================ gas

    /// One 64-member transition certificate (43 of 64 signing, 21 absent
    /// witnesses) plus one release to a fresh recipient, cold, in one batch:
    /// 21,000 + 40 gas per calldata byte + execution stays under 2^24
    /// (EIP-7825). The same certificate and release made directly are measured
    /// from the same state for the batch overhead.
    function test_SixtyFourMemberTransitionPlusReleaseFitsTheCap() public {
        Signer.TestSet memory big1 =
            Signer.makeUniformSet(harness, 1, keccak256("RelayBatchV2Test/64-member Set'1"), 64, 100);
        Signer.TestSet memory s2 = lineageSet(2);
        (B3FinalityVerifierV2 v64, B3VaultV2 vault64) = deployBridge();
        bootstrap(v64, 1, big1);
        vm.warp(block.timestamp + MIN_EPOCH);

        // Reserve by plain transfer and absorb (no deposit needed for a release).
        usdt.issue(500 * UNIT);
        usdt.transfer(address(vault64), 500 * UNIT);
        assertEq(vault64.absorb(), 500 * UNIT);

        address payee = address(0xFEE0FEE0);
        B3TypesV2.WithdrawalV2[] memory ws = new B3TypesV2.WithdrawalV2[](1);
        ws[0] = withdrawal(0, payee, 500 * UNIT, V);
        bytes32[] memory leaves = treeOf(vault64, ws);
        uint32[] memory absent = Signer.minQuorumAbsent(64);
        assertEq(absent.length, 21);
        Cert memory c = certWithRoot(CANONICAL, V_INDEX, 1, big1, s2, rootOf(leaves, 1), 0, absent);
        B3RelayBatch.ReleaseItem memory r = item(leaves, 1, ws[0]);

        // Direct calls first, each cold, from pre-encoded calldata.
        uint256 snapshot = vm.snapshotState();
        bytes memory certCall =
            abi.encodeCall(B3FinalityVerifierV2.submitCertificate, (c.fb, c.signingSet, c.successor, c.path, c.proof));
        bytes memory releaseCall = abi.encodeCall(B3VaultV2.release, (r.w, r.path));
        _coolAll(v64, vault64);
        uint256 g0 = gasleft();
        (bool ok,) = address(v64).call(certCall);
        uint256 directCert = g0 - gasleft();
        assertTrue(ok, "direct certificate");
        _coolAll(v64, vault64);
        g0 = gasleft();
        (ok,) = address(vault64).call(releaseCall);
        uint256 directRelease = g0 - gasleft();
        assertTrue(ok, "direct release");
        assertEq(usdt.balanceOf(payee), 500 * UNIT);
        assertTrue(vm.revertToState(snapshot));
        assertEq(usdt.balanceOf(payee), 0);
        assertEq(uint256(v64.currentEpoch()), 0);

        bytes memory data =
            abi.encodeCall(B3RelayBatch.relay, (v64, inputs(c), CERT_GAS, address(vault64), items(r), RELEASE_GAS));
        _coolAll(v64, vault64);
        g0 = gasleft();
        bytes memory ret;
        (ok, ret) = address(batch).call(data);
        uint256 execution = g0 - gasleft();
        assertTrue(ok, "batch call");
        (uint256 certsAccepted, uint256 releasesPaid) = abi.decode(ret, (uint256, uint256));
        assertEq(certsAccepted, 1);
        assertEq(releasesPaid, 1);
        assertEq(uint256(v64.currentEpoch()), 1);
        assertEq(v64.currentSetHash(), big1.setHash);
        assertEq(uint256(v64.acceptedRootHeight(rootOf(leaves, 1))), V);
        assertEq(usdt.balanceOf(payee), 500 * UNIT);
        assertBatchEmpty();

        uint256 conservative = 21_000 + data.length * 40 + execution;
        ConsoleV2.log("64-member transition cert, direct: calldata bytes", certCall.length);
        ConsoleV2.log("64-member transition cert, direct: execution gas", directCert);
        ConsoleV2.log("64-member transition cert, direct: conservative gas", 21_000 + certCall.length * 40 + directCert);
        ConsoleV2.log("release (cold, fresh recipient), direct: execution gas", directRelease);
        ConsoleV2.log(
            "release (cold, fresh recipient), direct: conservative gas",
            21_000 + releaseCall.length * 40 + directRelease
        );
        ConsoleV2.log("sum of the two direct executions", directCert + directRelease);
        ConsoleV2.log("batch (cert + release): calldata bytes", data.length);
        ConsoleV2.log("batch (cert + release): execution gas", execution);
        ConsoleV2.log("batch (cert + release): conservative gas", conservative);
        assertLt(conservative, EIP7825_TX_GAS_CAP, "EIP-7825 cap");
    }

    // ============================================================ helpers

    /// `proof` with its first absent-member witness moved off the curve: the
    /// lowest bit of y flips, which keeps x and (for all but a negligible set
    /// of points) the compressed sign bit, so only the G1ADD precompile can
    /// refuse it.
    function _offCurveAbsentWitness(bytes memory proof) private pure returns (bytes memory) {
        (bytes memory bitmap, bytes memory signature, bytes memory aggregate, BlsDigestProver.Absent[] memory absent) =
            abi.decode(proof, (bytes, bytes, bytes, BlsDigestProver.Absent[]));
        require(absent.length != 0, "no absent witness");
        absent[0].uncompressedPubkey[127] ^= 0x01;
        return abi.encode(bitmap, signature, aggregate, absent);
    }

    function _coolAll(B3FinalityVerifierV2 target, B3VaultV2 targetVault) private {
        vm.cool(address(target));
        vm.cool(address(prover));
        vm.cool(address(targetVault));
        vm.cool(address(usdt));
        vm.cool(address(batch));
    }
}
