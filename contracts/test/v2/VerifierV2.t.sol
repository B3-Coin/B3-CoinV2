// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {VerifierFixtureV2} from "./utils/VerifierFixtureV2.sol";
import {BlsTestSignerV2 as Signer} from "./utils/BlsTestSignerV2.sol";
import {B3Types} from "../../src/IB3FinalityProver.sol";
import {B3TypesV2} from "../../src/v2/B3TypesV2.sol";
import {B3FinalityVerifierV2} from "../../src/v2/B3FinalityVerifierV2.sol";
import {IBlsDigestProver} from "../../src/v2/IBlsDigestProver.sol";

/// B3FinalityVerifierV2 certificates, ancestry, root history, windows, the
/// deposit-close flag, conflict halts and the constructor guards (contracts
/// spec §2.3-§4). Every certificate is a real BLS proof from the in-EVM
/// signer; the fixture's chain model is in test/v2/utils/VerifierFixtureV2.sol.
contract VerifierV2Test is VerifierFixtureV2 {
    B3FinalityVerifierV2.BootstrapMode private constant K_OF_N = B3FinalityVerifierV2.BootstrapMode.K_OF_N;
    B3FinalityVerifierV2.BootstrapMode private constant START_CERT =
    B3FinalityVerifierV2.BootstrapMode.START_CERTIFICATE;

    // ================================================== ancestry and indices

    function test_AncestryAfterSkippedCertificates() public {
        (B3FinalityVerifierV2 v, Signer.TestSet memory s0, Signer.TestSet memory s1) = liveAt0(1);
        // Every third checkpoint is relayed; each proves the previous one by path.
        uint64 prev = 1;
        for (uint64 index = 4; index <= 16; index += 3) {
            Cert memory c = certify(CANONICAL, index, prev, s0, s1);
            submit(v, c);
            assertEq(uint256(v.latestIndex()), index);
            assertEq(uint256(v.latestHeight()), heightAt(index));
            assertEq(v.latestBlockHash(), hashAt(CANONICAL, index));
            assertEq(v.latestHistoryRoot(), historyRoot(CANONICAL, index));
            assertEq(v.latestCertId(), B3TypesV2.certId(c.fb));
            if (heightAt(index) >= V) {
                assertEq(uint256(v.acceptedRootHeight(c.fb.withdrawalRoot)), heightAt(index));
            }
            prev = index;
        }
        assertEq(uint256(v.latestBridgeHeight()), heightAt(16));
        assertEq(v.latestBridgeRoot(), withdrawalRootAt(CANONICAL, 16));
    }

    function test_BadIndexPathLineageAndMonotoneRejected() public {
        (B3FinalityVerifierV2 v, Signer.TestSet memory s0, Signer.TestSet memory s1) = liveAt0(1);
        submit(v, certify(CANONICAL, 5, 1, s0, s1));

        // Not above the latest index, or outside the tree.
        expectSubmitRevert(v, certify(CANONICAL, 5, 1, s0, s1), B3FinalityVerifierV2.BadCheckpointIndex.selector);
        expectSubmitRevert(v, certify(CANONICAL, 3, 1, s0, s1), B3FinalityVerifierV2.BadCheckpointIndex.selector);
        Cert memory c = certify(CANONICAL, 7, 5, s0, s1);
        c.fb.checkpointIndex = uint64(B3TypesV2.MAX_INDEX);
        expectSubmitRevert(v, c, B3FinalityVerifierV2.BadCheckpointIndex.selector);

        // A flipped sibling; the path of another position; a history that
        // does not contain the latest checkpoint (fork from index 3).
        c = certify(CANONICAL, 7, 5, s0, s1);
        c.path[0] = bytes32(uint256(c.path[0]) ^ 1);
        expectSubmitRevert(v, c, B3FinalityVerifierV2.BadAncestry.selector);
        c = certify(CANONICAL, 7, 5, s0, s1);
        c.path = ancestry(CANONICAL, 7, 4);
        expectSubmitRevert(v, c, B3FinalityVerifierV2.BadAncestry.selector);
        expectSubmitRevert(v, certify(3, 7, 5, s0, s1), B3FinalityVerifierV2.BadAncestry.selector);

        // Another lineage id.
        c = certify(CANONICAL, 7, 5, s0, s1);
        c.fb.lineageId = keccak256("another lineage");
        resign(c, s0);
        expectSubmitRevert(v, c, B3FinalityVerifierV2.BadLineage.selector);

        // A higher index but a height not above the latest.
        c = certify(CANONICAL, 7, 5, s0, s1);
        c.fb.height = heightAt(5);
        resign(c, s0);
        expectSubmitRevert(v, c, B3FinalityVerifierV2.NotMonotone.selector);
        c.fb.height = heightAt(5) - 1;
        resign(c, s0);
        expectSubmitRevert(v, c, B3FinalityVerifierV2.NotMonotone.selector);

        // Zero block or successor hash.
        c = certify(CANONICAL, 7, 5, s0, s1);
        c.fb.blockHash = bytes32(0);
        expectSubmitRevert(v, c, B3FinalityVerifierV2.BadFinalizedBlock.selector);

        submit(v, certify(CANONICAL, 7, 5, s0, s1));
        assertEq(uint256(v.latestIndex()), 7);
    }

    // ===================================================== withdrawal roots

    function test_WithdrawalRootOnBothSidesOfV() public {
        (B3FinalityVerifierV2 v, Signer.TestSet memory s0, Signer.TestSet memory s1) = liveAt0(1);
        assertEq(uint256(heightAt(V_INDEX)), V);

        // Below V: a non-zero root is premature; a zero root records nothing.
        Cert memory c = certify(CANONICAL, V_INDEX - 1, 1, s0, s1);
        c.fb.withdrawalRoot = keccak256("early root");
        resign(c, s0);
        expectSubmitRevert(v, c, B3FinalityVerifierV2.PrematureWithdrawalRoot.selector);
        submit(v, certify(CANONICAL, V_INDEX - 1, 1, s0, s1));
        assertEq(uint256(v.latestBridgeHeight()), 0);
        assertEq(v.latestBridgeRoot(), bytes32(0));

        // At V: a zero root is missing; the tree root is accepted from V on.
        c = certify(CANONICAL, V_INDEX, V_INDEX - 1, s0, s1);
        bytes32 root = c.fb.withdrawalRoot;
        c.fb.withdrawalRoot = bytes32(0);
        resign(c, s0);
        expectSubmitRevert(v, c, B3FinalityVerifierV2.MissingWithdrawalRoot.selector);
        submit(v, certify(CANONICAL, V_INDEX, V_INDEX - 1, s0, s1));
        assertEq(uint256(v.acceptedRootHeight(root)), V);
        assertEq(uint256(v.latestBridgeHeight()), V);
        assertEq(v.latestBridgeRoot(), root);
    }

    function test_UnqualifiedSigningSetRecordsNoRoot() public {
        Signer.TestSet memory s0 = lineageSet(0);
        Signer.TestSet memory u1 = unqualifiedSet(1);
        Signer.TestSet memory s2 = lineageSet(2);
        B3FinalityVerifierV2 v = deploy(K_OF_N, 0, s0.setHash);
        doInit(v, initArgs(v, 2, bootstrapKeys(), s0, u1, Signer.ids(3)));
        assertTrue(v.currentQualified());
        assertFalse(v.nextQualified(), "4 x 1 successor");

        // The unqualified set takes over at V: tracked, but no root.
        vm.warp(T0 + MIN_EPOCH);
        Cert memory c = certify(CANONICAL, V_INDEX, 2, u1, s2);
        submit(v, c);
        assertEq(uint256(v.currentEpoch()), 1);
        assertFalse(v.currentQualified());
        assertEq(uint256(v.acceptedRootHeight(c.fb.withdrawalRoot)), 0);
        assertEq(uint256(v.latestBridgeHeight()), 0);
        assertEq(v.latestBridgeRoot(), bytes32(0));
        assertFalse(v.depositsOpen());

        // A qualified successor records roots again.
        vm.warp(T0 + 2 * MIN_EPOCH);
        c = certify(CANONICAL, V_INDEX + 1, V_INDEX, s2, lineageSet(3));
        submit(v, c);
        assertEq(uint256(v.acceptedRootHeight(c.fb.withdrawalRoot)), heightAt(V_INDEX + 1));
        assertEq(uint256(v.acceptedRootHeight(withdrawalRootAt(CANONICAL, V_INDEX))), 0);
    }

    function test_RootHistoryClaimableAndWrittenOnce() public {
        (B3FinalityVerifierV2 v, Signer.TestSet memory s0, Signer.TestSet memory s1) = liveAt0(1);
        Cert memory c10 = certify(CANONICAL, V_INDEX, 1, s0, s1);
        submit(v, c10);
        Cert memory c11 = certify(CANONICAL, V_INDEX + 1, V_INDEX, s0, s1);
        submit(v, c11);
        bytes32 r10 = c10.fb.withdrawalRoot;
        bytes32 r11 = c11.fb.withdrawalRoot;
        assertTrue(r10 != r11);
        // Both stay claimable: the older root is not replaced by the newer.
        assertEq(uint256(v.acceptedRootHeight(r10)), V);
        assertEq(uint256(v.acceptedRootHeight(r11)), heightAt(V_INDEX + 1));
        assertEq(v.latestBridgeRoot(), r11);

        // A later certificate repeating r10 (no new burns) keeps r10's first height.
        Cert memory c12 = certify(CANONICAL, V_INDEX + 2, V_INDEX + 1, s0, s1);
        c12.fb.withdrawalRoot = r10;
        resign(c12, s0);
        submit(v, c12);
        assertEq(uint256(v.acceptedRootHeight(r10)), V, "written once");
        assertEq(v.latestBridgeRoot(), r10);
        assertEq(uint256(v.latestBridgeHeight()), heightAt(V_INDEX + 2));
        assertEq(uint256(v.acceptedRootHeight(r11)), heightAt(V_INDEX + 1));
    }

    // =============================================================== windows

    function test_TransitionSpacingEpochTooSoon() public {
        (B3FinalityVerifierV2 v,, Signer.TestSet memory s1) = liveAt0(1);
        Signer.TestSet memory s2 = lineageSet(2);
        Cert memory handover = certify(CANONICAL, 3, 1, s1, s2);
        vm.warp(T0 + MIN_EPOCH - 1);
        expectSubmitRevert(v, handover, B3FinalityVerifierV2.EpochTooSoon.selector);

        vm.warp(T0 + MIN_EPOCH);
        submit(v, handover);
        assertEq(uint256(v.currentEpoch()), 1);
        assertEq(v.currentSetHash(), s1.setHash);
        assertEq(v.nextSetHash(), s2.setHash);
        assertEq(v.setHashByEpoch(1), s1.setHash);
        assertEq(uint256(v.lastRotationTime()), T0 + MIN_EPOCH);
        assertEq(v.setExpiresAt(), T0 + MIN_EPOCH + LAG);

        // The next handover needs another full MIN_EPOCH.
        Cert memory again = certify(CANONICAL, 5, 3, s2, lineageSet(3));
        vm.warp(T0 + 2 * MIN_EPOCH - 1);
        expectSubmitRevert(v, again, B3FinalityVerifierV2.EpochTooSoon.selector);
        vm.warp(T0 + 2 * MIN_EPOCH);
        submit(v, again);
        assertEq(uint256(v.currentEpoch()), 2);
    }

    function test_CatchUpOverSeveralEpochsAtMinSpacing() public {
        (B3FinalityVerifierV2 v,, Signer.TestSet memory signer) = liveAt0(1);
        uint64 prev = 1;
        for (uint64 epoch = 1; epoch <= 5; ++epoch) {
            Signer.TestSet memory successor = lineageSet(epoch + 1);
            uint64 index = prev + 2;
            vm.warp(T0 + epoch * MIN_EPOCH);
            submit(v, certify(CANONICAL, index, prev, signer, successor));
            assertEq(uint256(v.currentEpoch()), epoch);
            assertEq(v.currentSetHash(), signer.setHash);
            assertEq(v.setHashByEpoch(epoch), signer.setHash);
            assertEq(v.nextSetHash(), successor.setHash);
            assertEq(uint256(v.lastRotationTime()), T0 + epoch * MIN_EPOCH);
            signer = successor;
            prev = index;
        }
    }

    function test_ExpiryAndSameEpochCertificatesDoNotRenew() public {
        (B3FinalityVerifierV2 v, Signer.TestSet memory s0, Signer.TestSet memory s1) = liveAt0(1);
        vm.warp(T0 + LAG / 2);
        submit(v, certify(CANONICAL, 3, 1, s0, s1));
        assertEq(uint256(v.lastRotationTime()), T0, "same-epoch certificate never renews");
        assertEq(uint256(v.lastCertificateTime()), T0 + LAG / 2);

        vm.warp(T0 + LAG);
        submit(v, certify(CANONICAL, 5, 3, s0, s1));

        vm.warp(T0 + LAG + 1);
        expectSubmitRevert(v, certify(CANONICAL, 7, 5, s0, s1), B3FinalityVerifierV2.SetExpired.selector);
        // The pinned next set cannot rotate after expiry either.
        expectSubmitRevert(v, certify(CANONICAL, 7, 5, s1, lineageSet(2)), B3FinalityVerifierV2.SetExpired.selector);
        // Nor can expired keys halt anything.
        expectConflictRevert(v, certify(5, 5, 3, s0, s1), 0, bytes32(0), B3FinalityVerifierV2.SetExpired.selector);
        assertEq(v.status(), B3TypesV2.STATUS_LIVE);
        assertFalse(v.depositsOpen());
    }

    function test_HandoverRenewsExpiry() public {
        (B3FinalityVerifierV2 v,, Signer.TestSet memory s1) = liveAt0(1);
        Signer.TestSet memory s2 = lineageSet(2);
        vm.warp(T0 + LAG);
        submit(v, certify(CANONICAL, 3, 1, s1, s2));
        assertEq(uint256(v.lastRotationTime()), T0 + LAG);
        vm.warp(T0 + 2 * LAG);
        submit(v, certify(CANONICAL, 5, 3, s1, s2));
        vm.warp(T0 + 2 * LAG + 1);
        expectSubmitRevert(v, certify(CANONICAL, 7, 5, s1, s2), B3FinalityVerifierV2.SetExpired.selector);
    }

    // ===================================================== epoch acceptance

    function test_EpochPlusTwoAndStaleEpochRejected() public {
        (B3FinalityVerifierV2 v, Signer.TestSet memory s0, Signer.TestSet memory s1) = liveAt0(1);
        Signer.TestSet memory s2 = lineageSet(2);
        vm.warp(T0 + MIN_EPOCH);
        expectSubmitRevert(
            v, certify(CANONICAL, 3, 1, s2, lineageSet(3)), B3FinalityVerifierV2.EpochNotAccepted.selector
        );
        submit(v, certify(CANONICAL, 3, 1, s1, s2));
        // Epoch 0 is over.
        expectSubmitRevert(v, certify(CANONICAL, 5, 3, s0, s1), B3FinalityVerifierV2.EpochNotAccepted.selector);
    }

    /// Invariant: nextSetHash != 0 whenever status == LIVE. initialize pins
    /// fb.validatorSetHash, every rotation pins the next one, and a
    /// same-epoch certificate must name the pinned successor, so the zero
    /// "unknown next set" state of spec §3.3 steps 2 and 10 never arises.
    /// An epoch+1 certificate is therefore always a handover by the pinned
    /// next set; the current set signing it is refused.
    function test_NextSetPinnedAfterInitializeAndEveryRotation() public {
        (B3FinalityVerifierV2 v, Signer.TestSet memory s0, Signer.TestSet memory s1) = liveAt0(1);
        assertEq(v.status(), B3TypesV2.STATUS_LIVE);
        assertEq(v.nextSetHash(), s1.setHash);
        assertNotEq(v.nextSetHash(), bytes32(0));

        // Same epoch: the pin is kept, never re-pinned.
        submit(v, certify(CANONICAL, 2, 1, s0, s1));
        assertEq(v.nextSetHash(), s1.setHash);
        expectSubmitRevert(
            v, certify(CANONICAL, 3, 2, s0, rogueSet(1)), B3FinalityVerifierV2.SuccessorMismatch.selector
        );

        // Epoch+1 signed by the current set is not a handover.
        vm.warp(T0 + MIN_EPOCH);
        Cert memory forged = certify(CANONICAL, 3, 2, s1, lineageSet(2));
        resign(forged, s0);
        forged.signingSet = Signer.copyHeader(s0.header);
        expectSubmitRevert(v, forged, B3FinalityVerifierV2.BadSigningSet.selector);

        Signer.TestSet memory signer = s1;
        uint64 prev = 2;
        for (uint64 epoch = 1; epoch <= 3; ++epoch) {
            Signer.TestSet memory successor = lineageSet(epoch + 1);
            vm.warp(T0 + epoch * MIN_EPOCH);
            submit(v, certify(CANONICAL, prev + 1, prev, signer, successor));
            assertEq(uint256(v.currentEpoch()), epoch);
            assertEq(v.nextSetHash(), successor.setHash);
            assertNotEq(v.nextSetHash(), bytes32(0));
            (,, uint32 nextCount, uint64 nextWeight) = v.setSummary();
            assertEq(uint256(nextCount), uint256(successor.header.validatorCount));
            assertEq(uint256(nextWeight), uint256(successor.header.totalWeight));
            signer = successor;
            prev = prev + 1;
        }
    }

    // ==================================================== successor, signer

    function test_SuccessorRules() public {
        (B3FinalityVerifierV2 v, Signer.TestSet memory s0, Signer.TestSet memory s1) = liveAt0(1);

        // A same-epoch certificate naming another successor than the pinned one.
        expectSubmitRevert(
            v, certify(CANONICAL, 3, 1, s0, rogueSet(1)), B3FinalityVerifierV2.SuccessorMismatch.selector
        );

        // Successor of the wrong epoch.
        expectSubmitRevert(v, certify(CANONICAL, 3, 1, s0, lineageSet(2)), B3FinalityVerifierV2.BadSuccessor.selector);

        // Successor header that is not the one fb names.
        Cert memory c = certify(CANONICAL, 3, 1, s0, s1);
        c.successor = rogueSet(1).header;
        expectSubmitRevert(v, c, B3FinalityVerifierV2.BadSuccessor.selector);

        // A ruleset-1 successor, correctly named and signed.
        B3Types.SetHeader memory r1 = Signer.copyHeader(s1.header);
        r1.rulesetVersion = 1;
        c = certify(CANONICAL, 3, 1, s0, s1);
        c.successor = r1;
        c.fb.validatorSetHash = B3Types.hashSetHeader(r1);
        resign(c, s0);
        expectSubmitRevert(v, c, B3FinalityVerifierV2.BadSuccessor.selector);

        submit(v, certify(CANONICAL, 3, 1, s0, s1));
        assertEq(v.nextSetHash(), s1.setHash);
    }

    function test_SigningSetRules() public {
        (B3FinalityVerifierV2 v, Signer.TestSet memory s0, Signer.TestSet memory s1) = liveAt0(1);
        Signer.TestSet memory s2 = lineageSet(2);

        // An epoch-0 statement under Set'1's header (same keys, other header).
        Cert memory c = certify(CANONICAL, 3, 1, s0, s1);
        c.signingSet = Signer.copyHeader(s1.header);
        expectSubmitRevert(v, c, B3FinalityVerifierV2.BadSigningSet.selector);

        // The bootstrap keys have no path after initialization.
        Signer.TestSet memory keys = bootstrapKeys();
        c = certify(CANONICAL, 3, 1, s0, s1);
        c.signingSet = keys.header;
        c.proof = signFb(keys, c.fb, Signer.none());
        expectSubmitRevert(v, c, B3FinalityVerifierV2.BadSigningSet.selector);

        // The right header with a proof from other keys.
        c = certify(CANONICAL, 3, 1, s0, s1);
        c.proof = signFb(rogueSet(0), c.fb, Signer.none());
        expectSubmitRevert(v, c, B3FinalityVerifierV2.ProofRejected.selector);

        // A handover presenting the current header instead of the pinned next.
        vm.warp(T0 + MIN_EPOCH);
        c = certify(CANONICAL, 3, 1, s1, s2);
        c.signingSet = Signer.copyHeader(s0.header);
        expectSubmitRevert(v, c, B3FinalityVerifierV2.BadSigningSet.selector);

        // Two of four fail the dual quorum.
        c = certify(CANONICAL, 3, 1, s1, s2);
        c.proof = signFb(s1, c.fb, Signer.ids(2, 3));
        expectSubmitRevert(v, c, B3FinalityVerifierV2.ProofRejected.selector);

        submit(v, certify(CANONICAL, 3, 1, s1, s2));
        assertEq(uint256(v.currentEpoch()), 1);
    }

    function test_V1TagRulesetOneAndOtherLineageRefused() public {
        (B3FinalityVerifierV2 v, Signer.TestSet memory s0, Signer.TestSet memory s1) = liveAt0(1);
        Cert memory c = certify(CANONICAL, V_INDEX, 1, s0, s1);

        // The same statement signed under the V1 tag and V1 layout.
        bytes32 tagV1 = sha256("B3/FINALITY/V1");
        bytes32 digestV1 = sha256(
            abi.encodePacked(
                tagV1,
                tagV1,
                DOMAIN,
                c.fb.height,
                c.fb.blockHash,
                c.fb.withdrawalRoot,
                c.fb.validatorSetHash,
                c.fb.epoch
            )
        );
        Cert memory v1 = certify(CANONICAL, V_INDEX, 1, s0, s1);
        v1.proof = Signer.sign(harness, s0, digestV1, Signer.none());
        expectSubmitRevert(v, v1, B3FinalityVerifierV2.ProofRejected.selector);

        // The V2 encoding under the V1 tag.
        bytes32 mixed = sha256(abi.encodePacked(tagV1, tagV1, DOMAIN, B3TypesV2.encode(c.fb)));
        v1.proof = Signer.sign(harness, s0, mixed, Signer.none());
        expectSubmitRevert(v, v1, B3FinalityVerifierV2.ProofRejected.selector);

        // Another chain domain under the V2 tag.
        bytes32 otherDomain = B3TypesV2.digest(keccak256("other domain"), c.fb);
        v1.proof = Signer.sign(harness, s0, otherDomain, Signer.none());
        expectSubmitRevert(v, v1, B3FinalityVerifierV2.ProofRejected.selector);

        // A ruleset-1 signing header.
        Cert memory r1 = certify(CANONICAL, V_INDEX, 1, s0, s1);
        r1.signingSet.rulesetVersion = 1;
        expectSubmitRevert(v, r1, B3FinalityVerifierV2.BadSigningSet.selector);

        // Another lineage.
        Cert memory other = certify(CANONICAL, V_INDEX, 1, s0, s1);
        other.fb.lineageId = B3TypesV2.lineageId(DOMAIN, G, genesisHash(), keccak256("another Set'0"));
        resign(other, s0);
        expectSubmitRevert(v, other, B3FinalityVerifierV2.BadLineage.selector);

        submit(v, c);
        assertEq(v.digestV2(c.fb), digestOf(c.fb));
    }

    // ===================================================== deposit-close flag

    function test_FlaggedCertificateClosesDepositsForGood() public {
        (B3FinalityVerifierV2 v,, Signer.TestSet memory s1) = liveAt0(1);
        Signer.TestSet memory s2 = lineageSet(2);
        vm.warp(T0 + REVIEW);
        submit(v, certify(CANONICAL, V_INDEX, 1, s1, s2));
        assertTrue(v.depositsOpen());

        Cert memory flagged = certify(CANONICAL, V_INDEX + 1, V_INDEX, s1, s2);
        flagged.fb.bridgeFlags = B3TypesV2.FLAG_DEPOSITS_CLOSED;
        resign(flagged, s1);
        bytes32 certId = B3TypesV2.certId(flagged.fb);
        vm.expectEmit(address(v));
        emit B3FinalityVerifierV2.DepositsClosedForGood(certId, heightAt(V_INDEX + 1));
        submit(v, flagged);
        assertTrue(v.depositsClosed());
        assertFalse(v.depositsOpen());
        // The flagged certificate's root is still accepted (earlier burns pay).
        assertEq(uint256(v.acceptedRootHeight(flagged.fb.withdrawalRoot)), heightAt(V_INDEX + 1));

        // Certificates continue, unflagged ones included; deposits stay closed.
        vm.recordLogs();
        Cert memory later = certify(CANONICAL, V_INDEX + 2, V_INDEX + 1, s1, s2);
        submit(v, later);
        assertEq(vm.getRecordedLogs().length, 1, "only Finalized, no second close event");
        assertEq(uint256(v.acceptedRootHeight(later.fb.withdrawalRoot)), heightAt(V_INDEX + 2));
        vm.warp(T0 + REVIEW + MIN_EPOCH);
        submit(v, certify(CANONICAL, V_INDEX + 3, V_INDEX + 2, s2, lineageSet(3)));
        assertEq(uint256(v.currentEpoch()), 2);
        assertTrue(v.depositsClosed());
        assertFalse(v.depositsOpen());
    }

    function test_OtherFlagBitsIgnored() public {
        (B3FinalityVerifierV2 v, Signer.TestSet memory s0, Signer.TestSet memory s1) = liveAt0(1);
        Cert memory c = certify(CANONICAL, V_INDEX, 1, s0, s1);
        c.fb.bridgeFlags = 0xfe;
        resign(c, s0);
        submit(v, c);
        assertFalse(v.depositsClosed());
    }

    // ================================================ rotations without reuse

    function test_RotationsThroughSetsSharingNoKey() public {
        (B3FinalityVerifierV2 v, Signer.TestSet memory s0, Signer.TestSet memory s1) = liveAt0(1);
        Signer.TestSet memory s2 = lineageSet(2);
        Signer.TestSet memory s3 = lineageSet(3);
        Signer.TestSet memory s4 = lineageSet(4);
        assertTrue(sharesKey(s0, s1), "epochs 0 and 1 share one binding");
        assertFalse(sharesKey(s1, s2));
        assertFalse(sharesKey(s2, s3));
        assertFalse(sharesKey(s3, s4));
        assertFalse(sharesKey(s1, s3));

        vm.warp(T0 + MIN_EPOCH);
        submit(v, certify(CANONICAL, 3, 1, s1, s2));
        vm.warp(T0 + 2 * MIN_EPOCH);
        submit(v, certify(CANONICAL, 5, 3, s2, s3));
        vm.warp(T0 + 3 * MIN_EPOCH);
        submit(v, certify(CANONICAL, V_INDEX, 5, s3, s4));
        assertEq(uint256(v.currentEpoch()), 3);
        assertEq(uint256(v.latestBridgeHeight()), V);

        // Set'2's keys cannot sign for epoch 3, under either header.
        Cert memory c = certify(CANONICAL, V_INDEX + 1, V_INDEX, s3, s4);
        c.proof = signFb(s2, c.fb, Signer.none());
        expectSubmitRevert(v, c, B3FinalityVerifierV2.ProofRejected.selector);
        c.signingSet = Signer.copyHeader(s2.header);
        expectSubmitRevert(v, c, B3FinalityVerifierV2.BadSigningSet.selector);
    }

    // ========================================================= fork, conflict

    function test_ForkCertificateLandingFirstBlocksCanonical() public {
        (B3FinalityVerifierV2 v, Signer.TestSet memory s0, Signer.TestSet memory s1) = liveAt0(1);
        submit(v, certify(CANONICAL, 4, 1, s0, s1));
        // A fork leaving the canonical chain at index 6 extends checkpoint 4: accepted.
        uint64 fork = 6;
        submit(v, certify(fork, 8, 4, s0, s1));
        assertEq(v.latestBlockHash(), hashAt(fork, 8));

        // The canonical certificate at 8 and its successors are now refused.
        expectSubmitRevert(v, certify(CANONICAL, 8, 4, s0, s1), B3FinalityVerifierV2.BadCheckpointIndex.selector);
        Cert memory canonical = certify(CANONICAL, V_INDEX, 8, s0, s1);
        expectSubmitRevert(v, canonical, B3FinalityVerifierV2.BadAncestry.selector);

        // That canonical certificate is conflict evidence: its history holds a
        // different leaf at the latest index (form 2).
        vm.expectEmit(address(v));
        emit B3FinalityVerifierV2.ConflictReported(B3TypesV2.certId(canonical.fb), V_INDEX, 2, address(this));
        reportConflict(v, canonical, heightAt(8), hashAt(CANONICAL, 8));
        assertEq(v.status(), B3TypesV2.STATUS_HALTED_CONFLICT);
    }

    function test_ConflictSameIndexHalts() public {
        (B3FinalityVerifierV2 v, Signer.TestSet memory s0, Signer.TestSet memory s1) = liveAt0(1);
        Cert memory accepted = certify(CANONICAL, 5, 1, s0, s1);
        submit(v, accepted);

        // The accepted statement itself is no evidence.
        expectConflictRevert(v, accepted, 0, bytes32(0), B3FinalityVerifierV2.NoConflict.selector);

        Cert memory fork = certify(5, 5, 1, s0, s1);
        vm.expectEmit(address(v));
        emit B3FinalityVerifierV2.ConflictReported(B3TypesV2.certId(fork.fb), 5, 1, address(this));
        reportConflict(v, fork, 0, bytes32(0));
        _assertHalted(v, s0, s1);
    }

    function test_ConflictLaterIndexHalts() public {
        (B3FinalityVerifierV2 v, Signer.TestSet memory s0, Signer.TestSet memory s1) = liveAt0(1);
        submit(v, certify(CANONICAL, 5, 1, s0, s1));

        // Canonical later statement: its leaf 5 is the latest leaf.
        Cert memory canonical = certify(CANONICAL, 8, 5, s0, s1);
        expectConflictRevert(v, canonical, heightAt(5), hashAt(CANONICAL, 5), B3FinalityVerifierV2.NoConflict.selector);

        // Fork from 5: its history has a different leaf at index 5.
        Cert memory fork = certify(5, 8, 5, s0, s1);
        // Naming the latest leaf, or a leaf the path does not place, proves nothing.
        expectConflictRevert(v, fork, heightAt(5), hashAt(CANONICAL, 5), B3FinalityVerifierV2.NoConflict.selector);
        expectConflictRevert(v, fork, heightAt(5), hashAt(5, 6), B3FinalityVerifierV2.NoConflict.selector);
        Cert memory wrongPath = certify(5, 8, 5, s0, s1);
        wrongPath.path = historyPath(CANONICAL, 8, 5);
        expectConflictRevert(v, wrongPath, heightAt(5), hashAt(5, 5), B3FinalityVerifierV2.NoConflict.selector);

        vm.expectEmit(address(v));
        emit B3FinalityVerifierV2.ConflictReported(B3TypesV2.certId(fork.fb), 8, 2, address(this));
        reportConflict(v, fork, heightAt(5), hashAt(5, 5));
        _assertHalted(v, s0, s1);
    }

    function test_ConflictEarlierIndexHalts() public {
        (B3FinalityVerifierV2 v, Signer.TestSet memory s0, Signer.TestSet memory s1) = liveAt0(1);
        submit(v, certify(CANONICAL, 8, 1, s0, s1));
        bytes32[32] memory underLatest = historyPath(CANONICAL, 8, 5);

        // The canonical statement at 5 agrees with leaf 5 of the stored history.
        Cert memory canonical = makeCert(s0, statement(CANONICAL, 5, 0, s1.setHash), s1, underLatest);
        expectConflictRevert(v, canonical, heightAt(5), hashAt(CANONICAL, 5), B3FinalityVerifierV2.NoConflict.selector);

        // An older fork statement at index 5 seen off Ethereum (e.g. on B3).
        Cert memory fork = makeCert(s0, statement(5, 5, 0, s1.setHash), s1, underLatest);
        // L must differ from the statement's own leaf and sit at index 5 under latestHistoryRoot.
        expectConflictRevert(v, fork, heightAt(5), hashAt(5, 5), B3FinalityVerifierV2.NoConflict.selector);
        expectConflictRevert(v, fork, heightAt(4), hashAt(CANONICAL, 4), B3FinalityVerifierV2.NoConflict.selector);

        vm.expectEmit(address(v));
        emit B3FinalityVerifierV2.ConflictReported(B3TypesV2.certId(fork.fb), 5, 3, address(this));
        reportConflict(v, fork, heightAt(5), hashAt(CANONICAL, 5));
        _assertHalted(v, s0, s1);
    }

    function test_HaltClosesAnOpenGateAndKeepsRoots() public {
        (B3FinalityVerifierV2 v,, Signer.TestSet memory s1) = liveAt0(1);
        Signer.TestSet memory s2 = lineageSet(2);
        vm.warp(T0 + REVIEW);
        Cert memory accepted = certify(CANONICAL, V_INDEX, 1, s1, s2);
        submit(v, accepted);
        assertTrue(v.depositsOpen());
        reportConflict(v, certify(V_INDEX, V_INDEX, 1, s1, s2), 0, bytes32(0));
        assertEq(v.status(), B3TypesV2.STATUS_HALTED_CONFLICT);
        assertFalse(v.depositsOpen());
        assertFalse(v.depositsClosed(), "the halt, not the flag, closes the gate");
        // Roots accepted before the halt stay readable for the vault (C-1).
        assertEq(uint256(v.acceptedRootHeight(accepted.fb.withdrawalRoot)), V);
    }

    function test_ConflictByPinnedNextSetHalts() public {
        (B3FinalityVerifierV2 v, Signer.TestSet memory s0, Signer.TestSet memory s1) = liveAt0(1);
        submit(v, certify(CANONICAL, 5, 1, s0, s1));
        // Set'1 (pinned next) signs a different epoch-1 statement at index 5.
        Cert memory fork = certify(5, 5, 1, s1, lineageSet(2));
        reportConflict(v, fork, 0, bytes32(0));
        assertEq(v.status(), B3TypesV2.STATUS_HALTED_CONFLICT);
    }

    function test_ConflictEvidenceFromWrongOrRotatedOutSetReverts() public {
        (B3FinalityVerifierV2 v,, Signer.TestSet memory s1) = liveAt0(1);
        Signer.TestSet memory s2 = lineageSet(2);
        Signer.TestSet memory s3 = lineageSet(3);
        vm.warp(T0 + MIN_EPOCH);
        submit(v, certify(CANONICAL, 3, 1, s1, s2));
        vm.warp(T0 + 2 * MIN_EPOCH);
        submit(v, certify(CANONICAL, 5, 3, s2, s3));
        assertEq(v.currentSetHash(), s2.setHash);
        assertEq(v.nextSetHash(), s3.setHash);

        // Rotated-out Set'1 (and Set'0), the bootstrap keys and an unrelated set cannot halt.
        Cert memory evidence = certify(5, 5, 3, s1, s2);
        expectConflictRevert(v, evidence, 0, bytes32(0), B3FinalityVerifierV2.BadSigningSet.selector);
        evidence = certify(5, 5, 3, lineageSet(0), s1);
        expectConflictRevert(v, evidence, 0, bytes32(0), B3FinalityVerifierV2.BadSigningSet.selector);
        Signer.TestSet memory keys = bootstrapKeys();
        evidence = certify(5, 5, 3, s2, s3);
        evidence.signingSet = keys.header;
        evidence.proof = signFb(keys, evidence.fb, Signer.none());
        expectConflictRevert(v, evidence, 0, bytes32(0), B3FinalityVerifierV2.BadSigningSet.selector);
        evidence = certify(5, 5, 3, rogueSet(2), s3);
        expectConflictRevert(v, evidence, 0, bytes32(0), B3FinalityVerifierV2.BadSigningSet.selector);

        // Current header with another set's proof; index 0; another lineage.
        evidence = certify(5, 5, 3, s2, s3);
        evidence.proof = signFb(rogueSet(2), evidence.fb, Signer.none());
        expectConflictRevert(v, evidence, 0, bytes32(0), B3FinalityVerifierV2.ProofRejected.selector);
        evidence = certify(5, 5, 3, s2, s3);
        evidence.fb.checkpointIndex = 0;
        expectConflictRevert(v, evidence, 0, bytes32(0), B3FinalityVerifierV2.BadCheckpointIndex.selector);
        evidence = certify(5, 5, 3, s2, s3);
        evidence.fb.lineageId = keccak256("another lineage");
        expectConflictRevert(v, evidence, 0, bytes32(0), B3FinalityVerifierV2.BadLineage.selector);
        assertEq(v.status(), B3TypesV2.STATUS_LIVE);

        // The current set's evidence halts.
        reportConflict(v, certify(5, 5, 3, s2, s3), 0, bytes32(0));
        assertEq(v.status(), B3TypesV2.STATUS_HALTED_CONFLICT);
    }

    // ======================================================== constructor

    function test_ConstructorProfileRWindowsOnlyOnRegtest() public {
        assertEq(block.chainid, 31_337);
        B3FinalityVerifierV2 v = deployWith(config(K_OF_N, 0, genesisSetHash, profileR()));
        assertEq(v.MIN_EPOCH_DURATION(), MIN_EPOCH);
        assertEq(v.MAX_EPOCH_LAG(), LAG);
        assertEq(v.DEPOSIT_CLOSE_AFTER(), CLOSE);
        assertEq(v.MAX_CERTIFICATE_AGE(), AGE);
        assertEq(v.REVIEW_WINDOW(), REVIEW);

        vm.chainId(1);
        _expectConfigRevert(config(K_OF_N, 0, genesisSetHash, profileR()), B3FinalityVerifierV2.BadWindows.selector);
        vm.chainId(11_155_111);
        _expectConfigRevert(config(K_OF_N, 0, genesisSetHash, profileR()), B3FinalityVerifierV2.BadWindows.selector);

        // The B-10 values (and the §5 seven-day deadline and review) on chain 1.
        vm.chainId(1);
        v = deployWith(config(K_OF_N, 0, genesisSetHash, profileB10()));
        assertEq(v.MIN_EPOCH_DURATION(), 21_600);
        assertEq(v.MAX_EPOCH_LAG(), 2_592_000);
        assertEq(v.DEPOSIT_CLOSE_AFTER(), 259_200);
        assertEq(v.MAX_CERTIFICATE_AGE(), 86_400);
        assertEq(v.INIT_DEADLINE(), T0 + 7 days);
        assertEq(v.REVIEW_WINDOW(), 7 days);
        vm.chainId(11_155_111);
        deployWith(config(START_CERT, 0, genesisSetHash, profileB10()));
    }

    function test_ConstructorEchoesConfig() public {
        B3FinalityVerifierV2.Config memory c = config(K_OF_N, 0, genesisSetHash, profileR());
        address[] memory five = new address[](5);
        for (uint256 i = 0; i < 5; ++i) {
            // forge-lint: disable-next-line(unsafe-typecast)
            five[i] = address(uint160(0x1000 + i));
        }
        B3FinalityVerifierV2 v = new B3FinalityVerifierV2(c, bootstrapHeader, five);
        assertEq(v.CHAIN_DOMAIN(), DOMAIN);
        assertEq(uint256(v.GENESIS_HEIGHT()), G);
        assertEq(v.GENESIS_BLOCK_HASH(), genesisHash());
        assertEq(v.GENESIS_SET_HASH(), genesisSetHash);
        assertEq(v.LINEAGE_ID(), lineageIdV);
        assertEq(uint256(v.START_EPOCH()), 0);
        assertEq(v.START_SET_HASH(), genesisSetHash);
        assertTrue(v.BOOTSTRAP_MODE() == K_OF_N);
        assertEq(v.BOOTSTRAP_SET_HASH(), bootstrapSetHash);
        assertEq(address(v.PROVER()), address(prover));
        assertEq(v.PROVER_CODE_HASH(), address(prover).codehash);
        assertEq(uint256(v.BRIDGE_ACTIVATION_HEIGHT()), V);
        assertEq(uint256(v.MIN_BRIDGE_VALIDATORS()), 4);
        assertEq(uint256(v.MAX_BRIDGE_VALIDATORS()), 64);
        assertEq(uint256(v.MIN_BRIDGE_TOTAL_WEIGHT()), 900);
        assertEq(v.WATCHER_COUNT(), 5);
        address[] memory list = v.watchers();
        assertEq(list.length, 5);
        for (uint256 i = 0; i < 5; ++i) {
            assertEq(list[i], five[i]);
            assertTrue(v.isWatcher(five[i]));
        }
        assertEq(v.status(), B3TypesV2.STATUS_UNINITIALIZED);
        assertFalse(v.depositsOpen());
    }

    function test_ConstructorConfigGuards() public {
        B3FinalityVerifierV2.Config memory c;
        bytes4 bad = B3FinalityVerifierV2.BadConfig.selector;

        c = _cfg();
        c.chainDomain = bytes32(0);
        _relineage(c);
        _expectConfigRevert(c, bad);
        c = _cfg();
        c.genesisHeight = 0;
        _relineage(c);
        _expectConfigRevert(c, bad);
        c = _cfg();
        c.genesisBlockHash = bytes32(0);
        _relineage(c);
        _expectConfigRevert(c, bad);
        c = _cfg();
        c.genesisSetHash = bytes32(0);
        c.startSetHash = bytes32(0);
        _relineage(c);
        _expectConfigRevert(c, bad);

        // Lineage id not recomputed from the pins.
        c = _cfg();
        c.lineageId = keccak256("lineage");
        _expectConfigRevert(c, bad);
        c = _cfg();
        c.genesisBlockHash = keccak256("other genesis block");
        _expectConfigRevert(c, bad);

        // Start set: zero, or epoch 0 with a hash other than GENESIS_SET_HASH.
        c = _cfg();
        c.startSetHash = bytes32(0);
        _expectConfigRevert(c, bad);
        c = _cfg();
        c.startSetHash = keccak256("other Set'0");
        _expectConfigRevert(c, bad);
        c = _cfg();
        c.startEpoch = 3;
        c.startSetHash = keccak256("Set'3");
        new B3FinalityVerifierV2(c, bootstrapHeader, defaultWatchers());

        // V must be above GENESIS_HEIGHT.
        c = _cfg();
        c.bridgeActivationHeight = G;
        _expectConfigRevert(c, bad);
        c.bridgeActivationHeight = G + 1;
        new B3FinalityVerifierV2(c, bootstrapHeader, defaultWatchers());

        // 4 <= min <= max <= 64; minimum weight non-zero.
        c = _cfg();
        c.minBridgeValidators = 3;
        _expectConfigRevert(c, bad);
        c = _cfg();
        c.minBridgeValidators = 10;
        c.maxBridgeValidators = 9;
        _expectConfigRevert(c, bad);
        c = _cfg();
        c.maxBridgeValidators = 65;
        _expectConfigRevert(c, bad);
        c = _cfg();
        c.minBridgeTotalWeight = 0;
        _expectConfigRevert(c, bad);
        c = _cfg();
        c.minBridgeValidators = 64;
        c.maxBridgeValidators = 64;
        new B3FinalityVerifierV2(c, bootstrapHeader, defaultWatchers());
    }

    function test_ConstructorWindowGuardsRegtest() public {
        bytes4 bad = B3FinalityVerifierV2.BadWindows.selector;
        B3FinalityVerifierV2.Config memory c;

        c = _cfg();
        c.maxCertificateAge = 0;
        _expectConfigRevert(c, bad);
        c = _cfg();
        c.maxCertificateAge = CLOSE;
        _expectConfigRevert(c, bad);
        c = _cfg();
        c.depositCloseAfter = LAG;
        _expectConfigRevert(c, bad);
        c = _cfg();
        c.minEpochDuration = 0;
        _expectConfigRevert(c, bad);
        c = _cfg();
        c.minEpochDuration = LAG + 1;
        _expectConfigRevert(c, bad);
        c = _cfg();
        c.maxEpochLag = uint256(type(uint40).max) + 1;
        c.initDeadline = block.timestamp + 1;
        _expectConfigRevert(c, bad);
        c = _cfg();
        c.initDeadline = block.timestamp;
        _expectConfigRevert(c, bad);
        c = _cfg();
        c.initDeadline = block.timestamp + LAG + 1;
        _expectConfigRevert(c, bad);
        c = _cfg();
        c.reviewWindow = 0;
        _expectConfigRevert(c, bad);
        c = _cfg();
        c.reviewWindow = LAG;
        _expectConfigRevert(c, bad);

        // Boundaries that hold: minEpoch == lag, deadline == now + lag, any small windows.
        c = _cfg();
        c.minEpochDuration = LAG;
        c.initDeadline = block.timestamp + LAG;
        new B3FinalityVerifierV2(c, bootstrapHeader, defaultWatchers());
        c = _cfg();
        c.minEpochDuration = 1;
        c.maxCertificateAge = 1;
        c.depositCloseAfter = 2;
        c.maxEpochLag = 3;
        c.reviewWindow = 2;
        c.initDeadline = block.timestamp + 3;
        new B3FinalityVerifierV2(c, bootstrapHeader, defaultWatchers());
    }

    function test_ConstructorWindowGuardsMainnetIds() public {
        bytes4 bad = B3FinalityVerifierV2.BadWindows.selector;
        vm.chainId(1);
        B3FinalityVerifierV2.Config memory c;

        c = _cfgB10();
        c.minEpochDuration = 3_599;
        _expectConfigRevert(c, bad);
        c.minEpochDuration = 3_600;
        new B3FinalityVerifierV2(c, bootstrapHeader, defaultWatchers());

        c = _cfgB10();
        c.depositCloseAfter = B10_LAG - 7 days + 1;
        _expectConfigRevert(c, bad);
        c.depositCloseAfter = B10_LAG - 7 days;
        new B3FinalityVerifierV2(c, bootstrapHeader, defaultWatchers());

        c = _cfgB10();
        c.depositCloseAfter = B10_AGE;
        _expectConfigRevert(c, bad);
        c = _cfgB10();
        c.maxCertificateAge = 0;
        _expectConfigRevert(c, bad);
        c = _cfgB10();
        c.minEpochDuration = B10_LAG + 1;
        _expectConfigRevert(c, bad);
        c = _cfgB10();
        c.depositCloseAfter = B10_LAG + 1;
        _expectConfigRevert(c, bad);
        c = _cfgB10();
        c.initDeadline = block.timestamp + B10_LAG + 1;
        _expectConfigRevert(c, bad);
        c = _cfgB10();
        c.reviewWindow = B10_LAG;
        _expectConfigRevert(c, bad);

        // The review window, which the K_OF_N trust argument relies on, has a
        // 7-day floor off regtest; regtest keeps any 0 < review < lag.
        c = _cfgB10();
        c.reviewWindow = 7 days - 1;
        _expectConfigRevert(c, bad);
        c.reviewWindow = 1;
        _expectConfigRevert(c, bad);
        vm.chainId(11_155_111);
        _expectConfigRevert(c, bad);
        c.reviewWindow = 7 days;
        new B3FinalityVerifierV2(c, bootstrapHeader, defaultWatchers());
        vm.chainId(31_337);
        c.reviewWindow = 1;
        new B3FinalityVerifierV2(c, bootstrapHeader, defaultWatchers());
    }

    /// Off regtest a K_OF_N deployment must name at least one watcher, since
    /// its review window only protects if someone can cancel. START_CERTIFICATE
    /// and regtest may run without watchers.
    function test_ConstructorKOfNNeedsAWatcherOffRegtest() public {
        address[] memory none = new address[](0);
        address[] memory one = new address[](1);
        one[0] = WATCHER_A;
        B3FinalityVerifierV2.Config memory kOfN = config(K_OF_N, 0, genesisSetHash, profileB10());
        B3FinalityVerifierV2.Config memory startCert = config(START_CERT, 0, genesisSetHash, profileB10());

        uint256[2] memory mainnetIds = [uint256(1), 11_155_111];
        for (uint256 i = 0; i < mainnetIds.length; ++i) {
            vm.chainId(mainnetIds[i]);
            vm.expectRevert(B3FinalityVerifierV2.BadWatchers.selector);
            new B3FinalityVerifierV2(kOfN, bootstrapHeader, none);
            assertEq(new B3FinalityVerifierV2(kOfN, bootstrapHeader, one).WATCHER_COUNT(), 1);
            assertEq(new B3FinalityVerifierV2(startCert, bootstrapHeader, none).WATCHER_COUNT(), 0);
        }

        vm.chainId(31_337);
        B3FinalityVerifierV2 v =
            new B3FinalityVerifierV2(config(K_OF_N, 0, genesisSetHash, profileR()), bootstrapHeader, none);
        assertEq(v.WATCHER_COUNT(), 0);
    }

    function test_ConstructorProverAndWatcherGuards() public {
        B3FinalityVerifierV2.Config memory c;
        bytes4 badProver = B3FinalityVerifierV2.BadProver.selector;

        c = _cfg();
        c.prover = IBlsDigestProver(address(0));
        _expectConfigRevert(c, badProver);
        c = _cfg();
        c.prover = IBlsDigestProver(address(0x1234));
        c.proverCodeHash = address(0x1234).codehash;
        _expectConfigRevert(c, badProver);
        c = _cfg();
        c.proverCodeHash = keccak256("other runtime");
        _expectConfigRevert(c, badProver);
        c = _cfg();
        c.proverCodeHash = bytes32(0);
        _expectConfigRevert(c, badProver);
        c = _cfg();
        c.prover = IBlsDigestProver(address(harness));
        _expectConfigRevert(c, badProver);

        bytes4 badWatchers = B3FinalityVerifierV2.BadWatchers.selector;
        address[] memory six = new address[](6);
        for (uint256 i = 0; i < 6; ++i) {
            // forge-lint: disable-next-line(unsafe-typecast)
            six[i] = address(uint160(0x2000 + i));
        }
        vm.expectRevert(badWatchers);
        new B3FinalityVerifierV2(_cfg(), bootstrapHeader, six);
        address[] memory withZero = new address[](2);
        withZero[0] = WATCHER_A;
        vm.expectRevert(badWatchers);
        new B3FinalityVerifierV2(_cfg(), bootstrapHeader, withZero);
        address[] memory duplicate = new address[](3);
        duplicate[0] = WATCHER_A;
        duplicate[1] = WATCHER_B;
        duplicate[2] = WATCHER_A;
        vm.expectRevert(badWatchers);
        new B3FinalityVerifierV2(_cfg(), bootstrapHeader, duplicate);
    }

    // ================================================================ views

    function test_FinalizedEventAndViews() public {
        (B3FinalityVerifierV2 v, Signer.TestSet memory s0, Signer.TestSet memory s1) = liveAt0(1);
        Cert memory c = certify(CANONICAL, V_INDEX, 1, s0, s1);
        bytes32 certId = B3TypesV2.certId(c.fb);
        assertEq(v.certIdOf(c.fb), certId);
        assertEq(v.digestV2(c.fb), digestOf(c.fb));
        assertEq(v.hashSetHeader(s0.header), s0.setHash);

        vm.warp(T0 + 100);
        vm.expectEmit(address(v));
        emit B3FinalityVerifierV2.Finalized(certId, 0, V, c.fb.withdrawalRoot, hex"07");
        submit(v, c);
        assertEq(uint256(v.lastCertificateTime()), T0 + 100);
        assertEq(v.depositsCloseAt(), T0 + CLOSE < T0 + 100 + AGE ? T0 + CLOSE : T0 + 100 + AGE);
        assertEq(v.setExpiresAt(), T0 + LAG);
        (uint32 cc, uint64 cw, uint32 nc, uint64 nw) = v.setSummary();
        assertEq(uint256(cc) + uint256(nc), 8);
        assertEq(uint256(cw) + uint256(nw), 2_000);
    }

    // ============================================================== helpers

    function _assertHalted(B3FinalityVerifierV2 v, Signer.TestSet memory s0, Signer.TestSet memory s1) private {
        assertEq(v.status(), B3TypesV2.STATUS_HALTED_CONFLICT);
        assertFalse(v.depositsOpen());
        uint64 next = v.latestIndex() + 4;
        expectSubmitRevert(v, certify(CANONICAL, next, v.latestIndex(), s0, s1), B3FinalityVerifierV2.NotLive.selector);
        expectConflictRevert(v, certify(7, 7, 1, s0, s1), 0, bytes32(0), B3FinalityVerifierV2.NotLive.selector);
        vm.prank(WATCHER_A);
        vm.expectRevert(B3FinalityVerifierV2.CancelClosed.selector);
        v.cancel();
    }

    function _cfg() private view returns (B3FinalityVerifierV2.Config memory) {
        return config(K_OF_N, 0, genesisSetHash, profileR());
    }

    function _cfgB10() private view returns (B3FinalityVerifierV2.Config memory) {
        return config(K_OF_N, 0, genesisSetHash, profileB10());
    }

    function _relineage(B3FinalityVerifierV2.Config memory c) private pure {
        c.lineageId = B3TypesV2.lineageId(c.chainDomain, c.genesisHeight, c.genesisBlockHash, c.genesisSetHash);
    }

    function _expectConfigRevert(B3FinalityVerifierV2.Config memory c, bytes4 selector) private {
        vm.expectRevert(selector);
        new B3FinalityVerifierV2(c, bootstrapHeader, defaultWatchers());
    }
}
