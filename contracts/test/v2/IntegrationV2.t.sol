// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {VmV2} from "./V2TestBase.sol";
import {RelayFixtureV2} from "./utils/RelayFixtureV2.sol";
import {BlsTestSignerV2 as Signer} from "./utils/BlsTestSignerV2.sol";
import {MerkleTreeV2} from "./utils/MerkleTreeV2.sol";
import {B3TypesV2} from "../../src/v2/B3TypesV2.sol";
import {B3FinalityVerifierV2} from "../../src/v2/B3FinalityVerifierV2.sol";
import {B3VaultV2} from "../../src/v2/B3VaultV2.sol";
import {B3RelayBatch} from "../../src/v2/B3RelayBatch.sol";

/// End to end on the real V2 stack and real BLS (regtest plan RT4, contracts
/// spec §3-§6, §9): BlsDigestProver, B3FinalityVerifierV2 (K_OF_N),
/// B3VaultV2 on MockTetherToken and B3RelayBatch, with certificates signed
/// by the in-EVM signer and withdrawal roots over real leaf V2 values.
///
/// - Route (a) then (d1): bootstrap, pre-V certificates with zero roots, the
///   epoch handover at V, an EOA deposit with V1's Deposit event, burns, one
///   certificate and the releases in one batch.
/// - B3's bridgeFlags bit 0: a flagged certificate closes direct deposits for
///   good while earlier leaves still release.
/// - Conflict halt on a second deployment of the same lineage: an accepted
///   leaf still releases (C-1), deposits and later certificates stop, and the
///   first deployment carries on.
contract IntegrationV2Test is RelayFixtureV2 {
    B3FinalityVerifierV2 private v;
    B3VaultV2 private vault;

    function setUp() public override {
        super.setUp();
        (v, vault) = deployBridge();
    }

    function test_RouteA_ThenD1_OnRealBls() public {
        Signer.TestSet memory s0 = lineageSet(0);
        Signer.TestSet memory s1 = lineageSet(1);
        Signer.TestSet memory s2 = lineageSet(2);

        // 1. K_OF_N bootstrap: 3 of the 4 bootstrap keys sign the epoch-0
        //    statement at checkpoint 1 (height 266 < V), naming Set'1.
        bootstrap(v, 1, s1);
        assertEq(v.status(), B3TypesV2.STATUS_LIVE);
        assertEq(uint256(v.currentEpoch()), 0);
        assertEq(v.currentSetHash(), s0.setHash);
        assertEq(v.nextSetHash(), s1.setHash);
        assertEq(uint256(v.lastCertificateTime()), 0);
        assertFalse(vault.depositGateOpen(), "bootstrap alone opens nothing");

        // 2. Pre-V certificates by Set'0, relayed together: roots are zero and nothing is claimable.
        Cert memory c4 = canonicalCert(4, 1, s0, s1, bytes32(0));
        Cert memory c7 = canonicalCert(7, 4, s0, s1, bytes32(0));
        B3RelayBatch.CertInput[] memory preV = new B3RelayBatch.CertInput[](2);
        preV[0] = toInput(c4);
        preV[1] = toInput(c7);
        (uint256 certsAccepted, uint256 releasesPaid) = relay(v, preV, address(0), noReleases());
        assertEq(certsAccepted, 2);
        assertEq(releasesPaid, 0);
        assertEq(uint256(v.latestIndex()), 7);
        assertEq(uint256(v.latestBridgeHeight()), 0);
        assertEq(uint256(v.acceptedRootHeight(bytes32(0))), 0);
        assertFalse(v.depositsOpen());
        fund(ALICE, address(vault), 1_000 * UNIT);
        vm.expectRevert(B3VaultV2.DepositClosed.selector);
        vm.prank(ALICE, ALICE);
        vault.deposit(1_000 * UNIT, B3_RECIPIENT);

        // 3. Past the review window and MIN_EPOCH_DURATION, Set'1 takes over
        //    at V with B3's (empty, non-zero) V2 withdrawal tree root.
        vm.warp(T0 + REVIEW);
        bytes32 emptyRoot = emptyTreeRoot();
        assertNotEq(emptyRoot, bytes32(0));
        (certsAccepted,) = relay(v, inputs(canonicalCert(V_INDEX, 7, s1, s2, emptyRoot)), address(0), noReleases());
        assertEq(certsAccepted, 1);
        assertEq(uint256(v.currentEpoch()), 1);
        assertEq(v.currentSetHash(), s1.setHash);
        assertEq(v.nextSetHash(), s2.setHash);
        assertEq(uint256(v.acceptedRootHeight(emptyRoot)), V);
        assertTrue(v.depositsOpen());
        assertTrue(vault.depositGateOpen());

        // 4. Route (a): approve (done by fund) + deposit from a codeless top-level EOA.
        vm.recordLogs();
        vm.prank(ALICE, ALICE);
        vault.deposit(1_000 * UNIT, B3_RECIPIENT);
        VmV2.Log[] memory vaultLogs = logsOf(vm.getRecordedLogs(), address(vault));
        assertEq(vaultLogs.length, 1);
        assertEq(vaultLogs[0].topics.length, 3);
        assertEq(vaultLogs[0].topics[0], DEPOSIT_TOPIC, "V1's Deposit topic");
        assertEq(vaultLogs[0].topics[1], bytes32(uint256(0)), "deposit id 0");
        assertEq(vaultLogs[0].topics[2], bytes32(uint256(uint160(address(usdt)))));
        assertEq(vaultLogs[0].data, abi.encode(1_000 * UNIT, B3_RECIPIENT));
        assertEq(vault.locked(), 1_000 * UNIT);
        assertEq(uint256(vault.nextDepositId()), 1);

        // 5. (d1): B3 burns two leaves above V; the next certificate carries
        //    their root and one batch relays it and pays both.
        B3TypesV2.WithdrawalV2[] memory ws = new B3TypesV2.WithdrawalV2[](2);
        ws[0] = withdrawal(0, BOB, 400 * UNIT, V + 3);
        ws[1] = withdrawal(1, CAROL, 250 * UNIT, heightAt(V_INDEX + 1) + 3);
        bytes32[] memory leaves = treeOf(vault, ws);
        bytes32 root = rootOf(leaves, 2);
        B3RelayBatch.CertInput[] memory certs = inputs(canonicalCert(V_INDEX + 2, V_INDEX, s1, s2, root));
        B3RelayBatch.ReleaseItem[] memory rel = items(item(leaves, 2, ws[0]), item(leaves, 2, ws[1]));

        vm.recordLogs();
        (certsAccepted, releasesPaid) = relay(v, certs, address(vault), rel);
        VmV2.Log[] memory all = vm.getRecordedLogs();
        assertEq(certsAccepted, 1);
        assertEq(releasesPaid, 2);
        assertEq(logsOf(all, address(batch)).length, 0, "nothing failed");
        vaultLogs = logsOf(all, address(vault));
        assertEq(vaultLogs.length, 2);
        assertEq(vaultLogs[0].topics[0], B3VaultV2.Withdrawn.selector);
        assertEq(vaultLogs[0].topics[3], bytes32(uint256(uint160(BOB))));
        assertEq(vaultLogs[1].topics[3], bytes32(uint256(uint160(CAROL))));
        assertEq(uint256(v.acceptedRootHeight(root)), heightAt(V_INDEX + 2));
        assertEq(usdt.balanceOf(BOB), 400 * UNIT);
        assertEq(usdt.balanceOf(CAROL), 250 * UNIT);
        assertEq(vault.locked(), 350 * UNIT);
        assertEq(usdt.balanceOf(address(vault)), 350 * UNIT);
        assertTrue(vault.released(0) && vault.released(1));
        assertTrue(v.depositsOpen(), "a same-epoch certificate keeps the gate open");

        // 6. Replaying the batch moves nothing: the certificate is stale, both ids are skipped.
        vm.recordLogs();
        (certsAccepted, releasesPaid) = relay(v, certs, address(vault), rel);
        VmV2.Log[] memory batchLogs = logsOf(vm.getRecordedLogs(), address(batch));
        assertEq(certsAccepted, 0);
        assertEq(releasesPaid, 0);
        assertEq(batchLogs.length, 3);
        assertCertificateFailed(batchLogs[0], 0, selectorError(B3FinalityVerifierV2.BadCheckpointIndex.selector));
        assertReleaseSkipped(batchLogs[1], 0, 0);
        assertReleaseSkipped(batchLogs[2], 1, 1);
        assertEq(usdt.balanceOf(BOB), 400 * UNIT);
        assertEq(vault.locked(), 350 * UNIT);
    }

    function test_FlaggedCertificateClosesDirectDepositsWhileEarlierLeavesRelease() public {
        (Signer.TestSet memory s1, Signer.TestSet memory s2) = _liveAtV(v);
        depositAs(vault, ALICE, 1_000 * UNIT);

        B3TypesV2.WithdrawalV2[] memory ws = new B3TypesV2.WithdrawalV2[](2);
        ws[0] = withdrawal(0, BOB, 400 * UNIT, V + 3);
        ws[1] = withdrawal(1, CAROL, 250 * UNIT, heightAt(V_INDEX + 2) + 2);
        bytes32[] memory leaves = treeOf(vault, ws);

        // Leaf 0 is certified before B3 closes burns.
        (uint256 certsAccepted,) =
            relay(v, inputs(canonicalCert(V_INDEX + 2, V_INDEX, s1, s2, rootOf(leaves, 1))), address(0), noReleases());
        assertEq(certsAccepted, 1);

        // B3 closes burns for good: the next certificate carries bridgeFlags bit 0 (and leaf 1).
        Cert memory flagged = certWithRoot(
            CANONICAL,
            V_INDEX + 3,
            V_INDEX + 2,
            s1,
            s2,
            rootOf(leaves, 2),
            B3TypesV2.FLAG_DEPOSITS_CLOSED,
            Signer.ids(3)
        );
        vm.expectEmit(address(v));
        emit B3FinalityVerifierV2.DepositsClosedForGood(B3TypesV2.certId(flagged.fb), heightAt(V_INDEX + 3));
        (certsAccepted,) = relay(v, inputs(flagged), address(0), noReleases());
        assertEq(certsAccepted, 1);
        assertTrue(v.depositsClosed());
        assertFalse(v.depositsOpen());
        assertFalse(vault.depositGateOpen());

        // A direct deposit from a codeless EOA is refused.
        fund(DAVE, address(vault), 100 * UNIT);
        vm.expectRevert(B3VaultV2.DepositClosed.selector);
        vm.prank(DAVE, DAVE);
        vault.deposit(100 * UNIT, B3_RECIPIENT);

        // Earlier leaves still release: leaf 0 under the older pre-flag root,
        // leaf 1 under the flagged certificate's root.
        uint256 releasesPaid;
        (certsAccepted, releasesPaid) =
            relay(v, noCerts(), address(vault), items(item(leaves, 1, ws[0]), item(leaves, 2, ws[1])));
        assertEq(releasesPaid, 2);
        assertEq(usdt.balanceOf(BOB), 400 * UNIT);
        assertEq(usdt.balanceOf(CAROL), 250 * UNIT);
        assertEq(vault.locked(), 350 * UNIT);

        // Certificates continue (even one without the flag); the close stays for good.
        (certsAccepted,) = relay(
            v, inputs(canonicalCert(V_INDEX + 4, V_INDEX + 3, s1, s2, rootOf(leaves, 2))), address(0), noReleases()
        );
        assertEq(certsAccepted, 1);
        assertEq(uint256(v.latestIndex()), V_INDEX + 4);
        assertEq(uint256(v.acceptedRootHeight(rootOf(leaves, 2))), heightAt(V_INDEX + 3), "written once");
        assertTrue(v.depositsClosed());
        assertFalse(vault.depositGateOpen());
        vm.expectRevert(B3VaultV2.DepositClosed.selector);
        vm.prank(DAVE, DAVE);
        vault.deposit(100 * UNIT, B3_RECIPIENT);
        assertEq(usdt.balanceOf(DAVE), 100 * UNIT, "nothing taken");
    }

    function test_ConflictHaltOnASecondDeploymentKeepsAcceptedLeavesPayable() public {
        // A second deployment on the same lineage, deployed beside the first.
        (B3FinalityVerifierV2 v2, B3VaultV2 vault2) = deployBridge();
        Signer.TestSet memory s1 = lineageSet(1);
        Signer.TestSet memory s2 = lineageSet(2);
        bootstrap(v, 1, s1);
        bootstrap(v2, 1, s1);
        vm.warp(T0 + REVIEW);

        // B3's certificates name only the lineage, so each serves both deployments.
        Cert memory c10 = canonicalCert(V_INDEX, 1, s1, s2, emptyTreeRoot());
        relay(v, inputs(c10), address(0), noReleases());
        relay(v2, inputs(c10), address(0), noReleases());
        depositAs(vault, ALICE, 1_000 * UNIT);
        depositAs(vault2, ALICE, 1_000 * UNIT);

        // One B3 withdrawal tree; each leaf names the vault that pays it.
        B3TypesV2.WithdrawalV2 memory w0 = withdrawal(0, BOB, 400 * UNIT, V + 3);
        B3TypesV2.WithdrawalV2 memory w1 = withdrawal(1, CAROL, 250 * UNIT, V + 4);
        B3TypesV2.WithdrawalV2 memory w2 = withdrawal(2, DAVE, 100 * UNIT, heightAt(V_INDEX + 2) + 3);
        bytes32[] memory leaves = new bytes32[](3);
        leaves[0] = leafOf(vault2, w0);
        leaves[1] = leafOf(vault, w1);
        leaves[2] = leafOf(vault2, w2);

        Cert memory c12 = canonicalCert(V_INDEX + 2, V_INDEX, s1, s2, rootOf(leaves, 2));
        relay(v, inputs(c12), address(0), noReleases());
        relay(v2, inputs(c12), address(0), noReleases());
        assertEq(uint256(v2.acceptedRootHeight(rootOf(leaves, 2))), heightAt(V_INDEX + 2));

        // Deployment 2: Set'1 also signed another statement at the accepted
        // index. The evidence halts it for good.
        Cert memory fork = certWithRoot(
            V_INDEX + 2, V_INDEX + 2, V_INDEX, s1, s2, keccak256("forked withdrawal root"), 0, Signer.ids(3)
        );
        uint8 sameIndexForm = v2.CONFLICT_SAME_INDEX();
        vm.expectEmit(address(v2));
        emit B3FinalityVerifierV2.ConflictReported(B3TypesV2.certId(fork.fb), V_INDEX + 2, sameIndexForm, address(this));
        v2.reportConflict(fork.fb, fork.signingSet, 0, bytes32(0), fork.path, fork.proof);
        assertEq(v2.status(), B3TypesV2.STATUS_HALTED_CONFLICT);
        assertFalse(v2.depositsOpen());
        assertFalse(vault2.depositGateOpen());
        fund(ALICE, address(vault2), 100 * UNIT);
        vm.expectRevert(B3VaultV2.DepositClosed.selector);
        vm.prank(ALICE, ALICE);
        vault2.deposit(100 * UNIT, B3_RECIPIENT);

        // One batch on deployment 2: the next certificate fails (NotLive), the
        // leaf accepted before the halt still releases (C-1), and leaf 2,
        // certified only after the halt, stays unpaid.
        Cert memory c13 = canonicalCert(V_INDEX + 3, V_INDEX + 2, s1, s2, rootOf(leaves, 3));
        vm.recordLogs();
        (uint256 certsAccepted, uint256 releasesPaid) =
            relay(v2, inputs(c13), address(vault2), items(item(leaves, 2, w0), item(leaves, 3, w2)));
        VmV2.Log[] memory batchLogs = logsOf(vm.getRecordedLogs(), address(batch));
        assertEq(certsAccepted, 0);
        assertEq(releasesPaid, 1);
        assertEq(batchLogs.length, 2);
        assertCertificateFailed(batchLogs[0], 0, selectorError(B3FinalityVerifierV2.NotLive.selector));
        assertReleaseFailed(batchLogs[1], 1, 2, selectorError(B3VaultV2.UnknownRoot.selector));
        assertEq(usdt.balanceOf(BOB), 400 * UNIT);
        assertEq(usdt.balanceOf(DAVE), 0);
        assertEq(vault2.locked(), 600 * UNIT);
        assertEq(uint256(v2.latestIndex()), V_INDEX + 2);

        // A leaf naming vault 2 is never payable by vault 1, although
        // verifier 1 accepted the same root.
        bytes32[32] memory path0 = MerkleTreeV2.path32(leaves, 2, 0);
        vm.expectRevert(B3VaultV2.UnknownRoot.selector);
        vault.release(w0, path0);

        // Deployment 1 carries on: it accepts the next certificate, pays its own leaf and keeps deposits open.
        (certsAccepted, releasesPaid) = relay(v, inputs(c13), address(vault), items(item(leaves, 3, w1)));
        assertEq(certsAccepted, 1);
        assertEq(releasesPaid, 1);
        assertEq(v.status(), B3TypesV2.STATUS_LIVE);
        assertTrue(v.depositsOpen());
        assertEq(usdt.balanceOf(CAROL), 250 * UNIT);
        assertEq(vault.locked(), 750 * UNIT);
    }

    // ============================================================ helpers

    /// Bootstrap at checkpoint 1, two pre-V certificates, then past review
    /// and MIN_EPOCH_DURATION the handover to Set'1 at V with the empty tree
    /// root. Returns Set'1 (current) and Set'2 (next).
    function _liveAtV(B3FinalityVerifierV2 target)
        private
        returns (Signer.TestSet memory s1, Signer.TestSet memory s2)
    {
        Signer.TestSet memory s0 = lineageSet(0);
        s1 = lineageSet(1);
        s2 = lineageSet(2);
        bootstrap(target, 1, s1);
        B3RelayBatch.CertInput[] memory preV = new B3RelayBatch.CertInput[](2);
        preV[0] = toInput(canonicalCert(4, 1, s0, s1, bytes32(0)));
        preV[1] = toInput(canonicalCert(7, 4, s0, s1, bytes32(0)));
        (uint256 certsAccepted,) = relay(target, preV, address(0), noReleases());
        assertEq(certsAccepted, 2);
        vm.warp(T0 + REVIEW);
        (certsAccepted,) =
            relay(target, inputs(canonicalCert(V_INDEX, 7, s1, s2, emptyTreeRoot())), address(0), noReleases());
        assertEq(certsAccepted, 1);
        assertTrue(target.depositsOpen());
    }
}
