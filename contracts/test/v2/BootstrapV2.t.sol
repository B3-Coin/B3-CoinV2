// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {VerifierFixtureV2} from "./utils/VerifierFixtureV2.sol";
import {BlsTestSignerV2 as Signer} from "./utils/BlsTestSignerV2.sol";
import {MerkleTreeV2} from "./utils/MerkleTreeV2.sol";
import {B3Types} from "../../src/IB3FinalityProver.sol";
import {B3TypesV2} from "../../src/v2/B3TypesV2.sol";
import {B3FinalityVerifierV2} from "../../src/v2/B3FinalityVerifierV2.sol";

/// B3FinalityVerifierV2 bootstrap (contracts spec §5, open issue OI-1) and the
/// watcher cancel. K_OF_N (default, B-11) and START_CERTIFICATE (alternative,
/// C-2) both run on real BLS signatures from the in-EVM signer.
contract BootstrapV2Test is VerifierFixtureV2 {
    B3FinalityVerifierV2.BootstrapMode private constant K_OF_N = B3FinalityVerifierV2.BootstrapMode.K_OF_N;
    B3FinalityVerifierV2.BootstrapMode private constant START_CERT =
    B3FinalityVerifierV2.BootstrapMode.START_CERTIFICATE;

    // ============================================================== K_OF_N

    function test_KOfN_ThreeOfFourAtEpoch0() public {
        Signer.TestSet memory s0 = lineageSet(0);
        Signer.TestSet memory s1 = lineageSet(1);
        B3FinalityVerifierV2 v = deploy(K_OF_N, 0, s0.setHash);
        assertEq(v.status(), B3TypesV2.STATUS_UNINITIALIZED);
        assertEq(v.START_SET_HASH(), genesisSetHash, "epoch 0 starts at Set'0");

        Init memory a = initArgs(2, bootstrapKeys(), s0, s1, Signer.ids(3));
        bytes32 certId = B3TypesV2.certId(a.fb);
        vm.expectEmit(address(v));
        emit B3FinalityVerifierV2.Initialized(certId, 0, heightAt(2), s1.setHash, hex"07");
        doInit(v, a);

        assertEq(v.status(), B3TypesV2.STATUS_LIVE);
        assertEq(uint256(v.currentEpoch()), 0);
        assertEq(v.currentSetHash(), s0.setHash);
        assertEq(v.nextSetHash(), s1.setHash);
        assertEq(v.setHashByEpoch(0), s0.setHash);
        assertEq(uint256(v.latestHeight()), heightAt(2));
        assertEq(uint256(v.latestIndex()), 2);
        assertEq(v.latestBlockHash(), hashAt(CANONICAL, 2));
        assertEq(v.latestCertId(), certId);
        assertEq(v.latestHistoryRoot(), historyRoot(CANONICAL, 2));
        assertEq(uint256(v.lastRotationTime()), T0);
        assertEq(uint256(v.initTime()), T0);
        assertEq(uint256(v.lastCertificateTime()), 0, "bootstrap is not a certificate");
        assertEq(uint256(v.latestBridgeHeight()), 0);
        assertEq(v.latestBridgeRoot(), bytes32(0));
        assertFalse(v.depositsClosed());
        assertFalse(v.depositsOpen());
        (uint32 cc, uint64 cw, uint32 nc, uint64 nw) = v.setSummary();
        assertEq(uint256(cc), 4);
        assertEq(uint256(cw), 1_000);
        assertEq(uint256(nc), 4);
        assertEq(uint256(nw), 1_000);
        assertEq(v.setExpiresAt(), T0 + LAG);
    }

    function test_KOfN_ThreeOfFourAtEpochK2() public {
        Signer.TestSet memory s2 = lineageSet(2);
        Signer.TestSet memory s3 = lineageSet(3);
        B3FinalityVerifierV2 v = deploy(K_OF_N, 2, s2.setHash);
        // Any three of the four keys: here keys 0, 2 and 3.
        doInit(v, initArgs(3, bootstrapKeys(), s2, s3, Signer.ids(1)));
        assertEq(uint256(v.currentEpoch()), 2);
        assertEq(v.currentSetHash(), s2.setHash);
        assertEq(v.nextSetHash(), s3.setHash);
        assertEq(v.setHashByEpoch(2), s2.setHash);
        assertEq(v.setHashByEpoch(0), bytes32(0), "earlier epochs are not tracked");

        // The lineage then continues normally: Set'2 signs, Set'3 takes over.
        submit(v, certify(CANONICAL, 5, 3, s2, s3));
        vm.warp(T0 + MIN_EPOCH);
        Signer.TestSet memory s4 = lineageSet(4);
        submit(v, certify(CANONICAL, 7, 5, s3, s4));
        assertEq(uint256(v.currentEpoch()), 3);
        assertEq(v.nextSetHash(), s4.setHash);
    }

    /// Documents the K_OF_N attestation scope (reported to the owner as a
    /// spec §5 gap). The bootstrap keys sign digestV2(fb) only, which binds
    /// the lineage, epoch k and the successor, but not START_SET_HASH for
    /// k > 0 nor the deployment: one published attestation initializes a
    /// second deployment whose deployer pinned another epoch-k set, and that
    /// set then certifies until the first handover. Only review of the pins
    /// against getfinalitylineage catches it. At k = 0 the start set is
    /// bound through the lineage (the constructor requires GENESIS_SET_HASH).
    function test_KOfN_AttestationDoesNotBindStartSetOrDeployment() public {
        Signer.TestSet memory s2 = lineageSet(2);
        Signer.TestSet memory s3 = lineageSet(3);
        Signer.TestSet memory pinnedElsewhere = rogueSet(2);
        B3FinalityVerifierV2 honest = deploy(K_OF_N, 2, s2.setHash);
        B3FinalityVerifierV2 other = deploy(K_OF_N, 2, pinnedElsewhere.setHash);

        Init memory a = initArgs(3, bootstrapKeys(), s2, s3, Signer.ids(1));
        doInit(honest, a);
        a.startSet = Signer.copyHeader(pinnedElsewhere.header);
        doInit(other, a);
        assertEq(other.currentSetHash(), pinnedElsewhere.setHash);
        assertEq(other.nextSetHash(), s3.setHash, "the successor is attested");
        submit(other, certify(CANONICAL, 5, 3, pinnedElsewhere, s3));
        assertEq(uint256(other.latestIndex()), 5);

        // k = 0: any start set other than the lineage's genesis set is refused.
        B3FinalityVerifierV2.Config memory c = config(K_OF_N, 0, rogueSet(0).setHash, profileR());
        address[] memory watchers = defaultWatchers();
        vm.expectRevert(B3FinalityVerifierV2.BadConfig.selector);
        new B3FinalityVerifierV2(c, bootstrapHeader, watchers);
    }

    function test_KOfN_FourOfFourAlsoAccepted() public {
        Signer.TestSet memory s0 = lineageSet(0);
        B3FinalityVerifierV2 v = deploy(K_OF_N, 0, s0.setHash);
        doInit(v, initArgs(1, bootstrapKeys(), s0, lineageSet(1), Signer.none()));
        assertEq(v.status(), B3TypesV2.STATUS_LIVE);
    }

    function test_KOfN_TwoOfFourRejected() public {
        Signer.TestSet memory s0 = lineageSet(0);
        B3FinalityVerifierV2 v = deploy(K_OF_N, 0, s0.setHash);
        Init memory a = initArgs(2, bootstrapKeys(), s0, lineageSet(1), Signer.ids(2, 3));
        expectInitRevert(v, a, B3FinalityVerifierV2.ProofRejected.selector);
        // A two-key proof with a bitmap claiming three signers fails the pairing.
        Signer.TestSet memory keys = bootstrapKeys();
        Signer.Parts memory p = Signer.signParts(harness, keys, digestOf(a.fb), Signer.ids(3));
        p.signature = Signer.aggregateSignature(harness, keys, digestOf(a.fb), Signer.ids(0, 1));
        a.proof = Signer.encode(p);
        expectInitRevert(v, a, B3FinalityVerifierV2.ProofRejected.selector);
        assertEq(v.status(), B3TypesV2.STATUS_UNINITIALIZED);
    }

    function test_KOfN_ThreeMemberBootstrapHeaderRefusedAtConstruction() public {
        // "Never 2-of-3": a 3-member header (quorum 3 of weight 3) is refused,
        // as is every other shape than 4 members, weight 4, quorum 3, epoch 0.
        B3FinalityVerifierV2.Config memory c = config(K_OF_N, 0, genesisSetHash, profileR());
        _expectBadBootstrap(c, Signer.makeUniformSet(harness, 0, SEED_BOOTSTRAP, 3, 1).header);
        _expectBadBootstrap(c, Signer.makeUniformSet(harness, 0, SEED_BOOTSTRAP, 5, 1).header);
        _expectBadBootstrap(c, Signer.makeUniformSet(harness, 0, SEED_BOOTSTRAP, 4, 2).header);
        _expectBadBootstrap(c, Signer.makeUniformSet(harness, 1, SEED_BOOTSTRAP, 4, 1).header);
        // Weight 4 and quorum 3 on 3 or 5 members: only the member count refuses them.
        uint64[] memory three = new uint64[](3);
        three[0] = 2;
        three[1] = 1;
        three[2] = 1;
        _expectBadBootstrap(c, Signer.makeSet(harness, 0, SEED_BOOTSTRAP, 3, three).header);
        uint64[] memory five = new uint64[](5);
        five[0] = 1;
        five[1] = 1;
        five[2] = 1;
        five[3] = 1;
        _expectBadBootstrap(c, Signer.makeSet(harness, 0, SEED_BOOTSTRAP, 5, five).header);
        // Four members of total weight 3 (quorum floor(6/3)+1 = 3): only the weight rule refuses it.
        uint64[] memory light = new uint64[](4);
        light[0] = 1;
        light[1] = 1;
        light[2] = 1;
        _expectBadBootstrap(c, Signer.makeSet(harness, 0, SEED_BOOTSTRAP, 4, light).header);

        // Ruleset 1, a wrong quorum field and a header that is not the pinned one.
        B3Types.SetHeader memory h = Signer.copyHeader(bootstrapHeader);
        h.rulesetVersion = 1;
        _expectBadBootstrap(c, h);
        h = Signer.copyHeader(bootstrapHeader);
        h.quorumWeight = 2;
        _expectBadBootstrap(c, h);
        h = Signer.copyHeader(bootstrapHeader);
        h.membersRoot = keccak256("other members");
        c.bootstrapSetHash = bootstrapSetHash;
        vm.expectRevert(B3FinalityVerifierV2.BadBootstrapSet.selector);
        new B3FinalityVerifierV2(c, h, defaultWatchers());

        // Zero pinned hash in K_OF_N mode.
        c.bootstrapSetHash = bytes32(0);
        vm.expectRevert(B3FinalityVerifierV2.BadBootstrapSet.selector);
        new B3FinalityVerifierV2(c, bootstrapHeader, defaultWatchers());
    }

    // ===================================================== shared rejections

    function test_WrongStartOrSigningSetRejected() public {
        Signer.TestSet memory s0 = lineageSet(0);
        Signer.TestSet memory s1 = lineageSet(1);
        Signer.TestSet memory keys = bootstrapKeys();
        B3FinalityVerifierV2 v = deploy(K_OF_N, 0, s0.setHash);

        // startSet is not the pinned START_SET_HASH.
        Init memory a = initArgs(2, keys, s0, s1, Signer.ids(3));
        a.startSet = s1.header;
        expectInitRevert(v, a, B3FinalityVerifierV2.BadStartSet.selector);

        // Another epoch-0 header (shape-valid, right epoch): only the hash pin refuses it,
        // so a bootstrap cannot install a forged current-set count or weight.
        a = initArgs(2, keys, s0, s1, Signer.ids(3));
        a.startSet = rogueSet(0).header;
        expectInitRevert(v, a, B3FinalityVerifierV2.BadStartSet.selector);
        a.startSet = unqualifiedSet(0).header;
        expectInitRevert(v, a, B3FinalityVerifierV2.BadStartSet.selector);
        a.startSet = Signer.copyHeader(s0.header);
        a.startSet.totalWeight = 1_500;
        a.startSet.quorumWeight = 1_001;
        expectInitRevert(v, a, B3FinalityVerifierV2.BadStartSet.selector);

        // startSet with the pinned hash's members but another epoch field.
        a = initArgs(2, keys, s0, s1, Signer.ids(3));
        a.startSet.epoch = 1;
        expectInitRevert(v, a, B3FinalityVerifierV2.BadStartSet.selector);

        // K_OF_N: the start set cannot sign in place of the bootstrap keys.
        a = initArgs(2, s0, s0, s1, Signer.none());
        expectInitRevert(v, a, B3FinalityVerifierV2.BadSigningSet.selector);

        // Bootstrap header altered (hash no longer the pin).
        a = initArgs(2, keys, s0, s1, Signer.ids(3));
        a.signingSet.membersRoot = keccak256("other");
        expectInitRevert(v, a, B3FinalityVerifierV2.BadSigningSet.selector);

        // Successor not named by fb, of the wrong epoch, or ruleset 1.
        a = initArgs(2, keys, s0, s1, Signer.ids(3));
        a.successor = lineageSet(2).header;
        expectInitRevert(v, a, B3FinalityVerifierV2.BadSuccessor.selector);
        B3Types.SetHeader memory r1 = Signer.copyHeader(s1.header);
        r1.rulesetVersion = 1;
        a = initArgs(2, keys, s0, s1, Signer.ids(3));
        a.successor = r1;
        a.fb.validatorSetHash = B3Types.hashSetHeader(r1);
        a.proof = signFb(keys, a.fb, Signer.ids(3));
        expectInitRevert(v, a, B3FinalityVerifierV2.BadSuccessor.selector);

        // Statement epoch other than START_EPOCH.
        a = initArgs(2, keys, s0, s1, Signer.ids(3));
        a.fb.epoch = 1;
        expectInitRevert(v, a, B3FinalityVerifierV2.EpochNotAccepted.selector);

        // Other lineage; zero block hash; zero successor hash.
        a = initArgs(2, keys, s0, s1, Signer.ids(3));
        a.fb.lineageId = keccak256("another lineage");
        expectInitRevert(v, a, B3FinalityVerifierV2.BadLineage.selector);
        a = initArgs(2, keys, s0, s1, Signer.ids(3));
        a.fb.blockHash = bytes32(0);
        expectInitRevert(v, a, B3FinalityVerifierV2.BadFinalizedBlock.selector);
        a = initArgs(2, keys, s0, s1, Signer.ids(3));
        a.fb.validatorSetHash = bytes32(0);
        expectInitRevert(v, a, B3FinalityVerifierV2.BadFinalizedBlock.selector);

        assertEq(v.status(), B3TypesV2.STATUS_UNINITIALIZED);
        doInit(v, initArgs(2, keys, s0, s1, Signer.ids(3)));
        assertEq(v.status(), B3TypesV2.STATUS_LIVE);
    }

    function test_BadLeaf0PathOrIndexRejected() public {
        Signer.TestSet memory s0 = lineageSet(0);
        Signer.TestSet memory s1 = lineageSet(1);
        Signer.TestSet memory keys = bootstrapKeys();
        B3FinalityVerifierV2 v = deploy(K_OF_N, 0, s0.setHash);

        // A flipped sibling.
        Init memory a = initArgs(4, keys, s0, s1, Signer.ids(3));
        a.leaf0Path[1] = bytes32(uint256(a.leaf0Path[1]) ^ 1);
        expectInitRevert(v, a, B3FinalityVerifierV2.BadAncestry.selector);

        // The path of leaf 1, not leaf 0: genesis is not at index 0 under this root.
        a = initArgs(4, keys, s0, s1, Signer.ids(3));
        a.leaf0Path = historyPath(CANONICAL, 4, 1);
        expectInitRevert(v, a, B3FinalityVerifierV2.BadAncestry.selector);

        // A history whose leaf 0 is not historyLeaf(GENESIS_HEIGHT, GENESIS_BLOCK_HASH).
        a = initArgs(4, keys, s0, s1, Signer.ids(3));
        bytes32[] memory leaves = historyLeaves(CANONICAL, 4);
        leaves[0] = B3TypesV2.historyLeaf(G + 1, genesisHash());
        a.fb.checkpointHistoryRoot = _root(leaves);
        a.leaf0Path = _path(leaves, 0);
        a.proof = signFb(keys, a.fb, Signer.ids(3));
        expectInitRevert(v, a, B3FinalityVerifierV2.BadAncestry.selector);

        // Index 0 (B3 never signs it) and index 2^32.
        a = initArgs(4, keys, s0, s1, Signer.ids(3));
        a.fb.checkpointIndex = 0;
        expectInitRevert(v, a, B3FinalityVerifierV2.BadCheckpointIndex.selector);
        a.fb.checkpointIndex = uint64(B3TypesV2.MAX_INDEX);
        expectInitRevert(v, a, B3FinalityVerifierV2.BadCheckpointIndex.selector);

        // A proof over another statement (index 3) under this one.
        a = initArgs(4, keys, s0, s1, Signer.ids(3));
        a.proof = initArgs(3, keys, s0, s1, Signer.ids(3)).proof;
        expectInitRevert(v, a, B3FinalityVerifierV2.ProofRejected.selector);
    }

    /// OI-1: the bootstrap statement must lie strictly between GENESIS_HEIGHT
    /// and V, with a zero root and zero flags, in both modes.
    function test_OI1_StatementAtOrAboveVRefusedInBothModes() public {
        Signer.TestSet memory s0 = lineageSet(0);
        Signer.TestSet memory s1 = lineageSet(1);
        Signer.TestSet memory keys = bootstrapKeys();
        B3FinalityVerifierV2 kOfN = deploy(K_OF_N, 0, s0.setHash);
        B3FinalityVerifierV2 startCert = deploy(START_CERT, 0, s0.setHash);

        // At V (index 10) and above, properly signed: the root is non-zero by B3's rule.
        uint64[2] memory indices = [V_INDEX, V_INDEX + 3];
        for (uint256 i = 0; i < 2; ++i) {
            Init memory a = initArgs(indices[i], keys, s0, s1, Signer.ids(3));
            assertTrue(a.fb.withdrawalRoot != bytes32(0));
            expectInitRevert(kOfN, a, B3FinalityVerifierV2.BadStartHeight.selector);
            Init memory b = initArgs(indices[i], s0, s0, s1, Signer.ids(3));
            expectInitRevert(startCert, b, B3FinalityVerifierV2.BadStartHeight.selector);
            // Even with the root zeroed and re-signed.
            b.fb.withdrawalRoot = bytes32(0);
            b.proof = signFb(s0, b.fb, Signer.none());
            expectInitRevert(startCert, b, B3FinalityVerifierV2.BadStartHeight.selector);
        }

        // At or below GENESIS_HEIGHT.
        Init memory low = initArgs(1, keys, s0, s1, Signer.ids(3));
        low.fb.height = G;
        low.proof = signFb(keys, low.fb, Signer.ids(3));
        expectInitRevert(kOfN, low, B3FinalityVerifierV2.BadStartHeight.selector);

        // Below V with a non-zero root, or with any flag bit.
        Init memory rooted = initArgs(V_INDEX - 1, keys, s0, s1, Signer.ids(3));
        rooted.fb.withdrawalRoot = keccak256("claimable root");
        rooted.proof = signFb(keys, rooted.fb, Signer.ids(3));
        expectInitRevert(kOfN, rooted, B3FinalityVerifierV2.PrematureWithdrawalRoot.selector);
        Init memory flagged = initArgs(V_INDEX - 1, s0, s0, s1, Signer.ids(3));
        flagged.fb.bridgeFlags = B3TypesV2.FLAG_DEPOSITS_CLOSED;
        flagged.proof = signFb(s0, flagged.fb, Signer.none());
        expectInitRevert(startCert, flagged, B3FinalityVerifierV2.BadBridgeFlags.selector);
        flagged.fb.bridgeFlags = 0x80;
        flagged.proof = signFb(s0, flagged.fb, Signer.none());
        expectInitRevert(startCert, flagged, B3FinalityVerifierV2.BadBridgeFlags.selector);

        // The last checkpoint below V is accepted in both modes; no root is recorded.
        Init memory last = initArgs(V_INDEX - 1, keys, s0, s1, Signer.ids(3));
        doInit(kOfN, last);
        doInit(startCert, initArgs(V_INDEX - 1, s0, s0, s1, Signer.ids(3)));
        assertEq(uint256(kOfN.latestBridgeHeight()), 0);
        assertEq(uint256(startCert.latestBridgeHeight()), 0);
        assertFalse(kOfN.depositsClosed());
    }

    function test_DeadlineAndSecondInitialize() public {
        Signer.TestSet memory s0 = lineageSet(0);
        Signer.TestSet memory s1 = lineageSet(1);
        Signer.TestSet memory keys = bootstrapKeys();
        B3FinalityVerifierV2 v = deploy(K_OF_N, 0, s0.setHash);
        assertEq(v.INIT_DEADLINE(), T0 + INIT_WINDOW);

        Init memory a = initArgs(2, keys, s0, s1, Signer.ids(3));
        vm.warp(T0 + INIT_WINDOW + 1);
        expectInitRevert(v, a, B3FinalityVerifierV2.InitDeadlinePassed.selector);

        // Exactly at the deadline is still in time.
        vm.warp(T0 + INIT_WINDOW);
        doInit(v, a);
        assertEq(uint256(v.initTime()), T0 + INIT_WINDOW);

        expectInitRevert(v, a, B3FinalityVerifierV2.AlreadyInitialized.selector);
        expectInitRevert(v, initArgs(3, keys, s0, s1, Signer.ids(0)), B3FinalityVerifierV2.AlreadyInitialized.selector);
    }

    function test_DepositsWaitForOrdinaryCertificateAfterReview() public {
        (B3FinalityVerifierV2 v, Signer.TestSet memory s0, Signer.TestSet memory s1) = liveAt0(2);
        assertEq(uint256(v.lastCertificateTime()), 0);
        assertFalse(v.depositsOpen(), "bootstrap alone");

        // An ordinary certificate at V during review: root accepted, gate shut.
        submit(v, certify(CANONICAL, V_INDEX, 2, s0, s1));
        assertEq(uint256(v.latestBridgeHeight()), V);
        assertEq(uint256(v.lastCertificateTime()), T0);
        assertTrue(v.currentQualified() && v.nextQualified());
        assertFalse(v.depositsOpen(), "review window");

        // Review over: the old rotation and certificate are now too old.
        vm.warp(T0 + REVIEW);
        assertFalse(v.depositsOpen(), "stale after review");

        // A handover after review refreshes both clocks: deposits open.
        Signer.TestSet memory s2 = lineageSet(2);
        submit(v, certify(CANONICAL, V_INDEX + 1, V_INDEX, s1, s2));
        assertTrue(v.depositsOpen());
        assertEq(v.depositsCloseAt(), T0 + REVIEW + AGE);

        // The gate closes as the certificate ages, and reopens with a keep-alive.
        vm.warp(T0 + REVIEW + AGE + 1);
        assertFalse(v.depositsOpen(), "certificate too old");
        submit(v, certify(CANONICAL, V_INDEX + 2, V_INDEX + 1, s1, s2));
        assertTrue(v.depositsOpen());
        // A rotation older than DEPOSIT_CLOSE_AFTER closes it although certificates continue.
        vm.warp(T0 + REVIEW + CLOSE + 1);
        submit(v, certify(CANONICAL, V_INDEX + 3, V_INDEX + 2, s1, s2));
        assertFalse(v.depositsOpen(), "rotation too old");
    }

    function test_DepositsNeedReviewEvenWithFreshCertificates() public {
        (B3FinalityVerifierV2 v, Signer.TestSet memory s0, Signer.TestSet memory s1) = liveAt0(2);
        Signer.TestSet memory s2 = lineageSet(2);
        submit(v, certify(CANONICAL, V_INDEX - 1, 2, s0, s1));
        vm.warp(T0 + MIN_EPOCH);
        submit(v, certify(CANONICAL, V_INDEX, V_INDEX - 1, s1, s2));
        // Everything but the review window holds.
        assertTrue(v.currentQualified() && v.nextQualified());
        assertEq(uint256(v.latestBridgeHeight()), V);
        assertEq(uint256(v.lastCertificateTime()), T0 + MIN_EPOCH);
        assertEq(uint256(v.lastRotationTime()), T0 + MIN_EPOCH);
        assertFalse(v.depositsOpen());
    }

    /// The contract sees only the header, not member weights: a 4-key header
    /// of weight 4 with uneven member weights passes construction (as in V1).
    /// The prover's headcount quorum, floor(2*4/3)+1 = 3, still demands three
    /// distinct keys, so no two bootstrap quorums are disjoint. The deployment
    /// manifest's member rows are what reviewers check for equal weights.
    function test_KOfN_MemberWeightsAreNotVisibleButHeadcountStaysThree() public {
        uint64[] memory uneven = new uint64[](4);
        uneven[0] = 2;
        uneven[1] = 1;
        uneven[2] = 1;
        uneven[3] = 0;
        Signer.TestSet memory keys = Signer.makeSet(harness, 0, SEED_BOOTSTRAP, 4, uneven);
        B3FinalityVerifierV2.Config memory c = config(K_OF_N, 0, genesisSetHash, profileR());
        c.bootstrapSetHash = keys.setHash;
        B3FinalityVerifierV2 v = new B3FinalityVerifierV2(c, keys.header, defaultWatchers());

        Signer.TestSet memory s0 = lineageSet(0);
        Signer.TestSet memory s1 = lineageSet(1);
        // Keys 0 and 1 hold weight 3 = quorumWeight, but two of four fail the headcount.
        expectInitRevert(v, initArgs(2, keys, s0, s1, Signer.ids(2, 3)), B3FinalityVerifierV2.ProofRejected.selector);
        // Keys 1, 2 and 3 are three of four but weigh 2 < 3.
        expectInitRevert(v, initArgs(2, keys, s0, s1, Signer.ids(0)), B3FinalityVerifierV2.ProofRejected.selector);
        doInit(v, initArgs(2, keys, s0, s1, Signer.ids(3)));
        assertEq(v.status(), B3TypesV2.STATUS_LIVE);
    }

    /// Each remaining gate term on its own: a fresh handover after review with
    /// everything else satisfied, except one term.
    function test_GateNeedsBridgeRootFromV() public {
        (B3FinalityVerifierV2 v,, Signer.TestSet memory s1) = liveAt0(2);
        Signer.TestSet memory s2 = lineageSet(2);
        vm.warp(T0 + REVIEW);
        submit(v, certify(CANONICAL, V_INDEX - 1, 2, s1, s2));
        assertTrue(v.currentQualified() && v.nextQualified());
        assertEq(uint256(v.latestBridgeHeight()), 0);
        assertFalse(v.depositsOpen(), "no root from V yet");
        submit(v, certify(CANONICAL, V_INDEX, V_INDEX - 1, s1, s2));
        assertTrue(v.depositsOpen());
    }

    function test_GateNeedsQualifiedNextSet() public {
        (B3FinalityVerifierV2 v,, Signer.TestSet memory s1) = liveAt0(2);
        vm.warp(T0 + REVIEW);
        submit(v, certify(CANONICAL, V_INDEX, 2, s1, unqualifiedSet(2)));
        assertEq(uint256(v.latestBridgeHeight()), V);
        assertTrue(v.currentQualified());
        assertFalse(v.nextQualified());
        assertFalse(v.depositsOpen(), "scheduled handover to an unqualified set");
    }

    function test_GateNeedsQualifiedCurrentSet() public {
        Signer.TestSet memory s0 = lineageSet(0);
        Signer.TestSet memory u1 = unqualifiedSet(1);
        B3FinalityVerifierV2 v = deploy(K_OF_N, 0, s0.setHash);
        doInit(v, initArgs(2, bootstrapKeys(), s0, u1, Signer.ids(3)));
        // Set'0 (qualified) certifies a root at V; then the unqualified set takes over.
        submit(v, certify(CANONICAL, V_INDEX, 2, s0, u1));
        vm.warp(T0 + REVIEW);
        submit(v, certify(CANONICAL, V_INDEX + 1, V_INDEX, u1, lineageSet(2)));
        assertEq(uint256(v.latestBridgeHeight()), V);
        assertFalse(v.currentQualified());
        assertTrue(v.nextQualified());
        assertFalse(v.depositsOpen(), "unqualified current set");
    }

    /// A START_SET_HASH pinned to a header of another epoch than START_EPOCH
    /// (deployer error) can never initialize.
    function test_StartSetEpochMustEqualStartEpoch() public {
        Signer.TestSet memory s2 = lineageSet(2);
        Signer.TestSet memory s3 = lineageSet(3);
        B3FinalityVerifierV2 v = deploy(START_CERT, 2, s3.setHash);
        Init memory a = initArgs(4, s3, s2, s3, Signer.none());
        a.startSet = Signer.copyHeader(s3.header);
        a.fb.epoch = 2;
        a.proof = signFb(s3, a.fb, Signer.none());
        expectInitRevert(v, a, B3FinalityVerifierV2.BadStartSet.selector);
    }

    // ===================================================== START_CERTIFICATE

    function test_StartCertificate_OrdinaryStartSetCertificate() public {
        Signer.TestSet memory s0 = lineageSet(0);
        Signer.TestSet memory s1 = lineageSet(1);
        B3FinalityVerifierV2 v = deploy(START_CERT, 0, s0.setHash);
        assertEq(v.BOOTSTRAP_SET_HASH(), bytes32(0));
        assertTrue(v.BOOTSTRAP_MODE() == START_CERT);

        // Bootstrap keys: their header is refused, and their proof under the start header fails.
        Init memory a = initArgs(2, bootstrapKeys(), s0, s1, Signer.ids(3));
        expectInitRevert(v, a, B3FinalityVerifierV2.BadSigningSet.selector);
        a.signingSet = s0.header;
        expectInitRevert(v, a, B3FinalityVerifierV2.ProofRejected.selector);
        // A 2-of-4 start-set proof fails the dual quorum.
        expectInitRevert(v, initArgs(2, s0, s0, s1, Signer.ids(1, 2)), B3FinalityVerifierV2.ProofRejected.selector);

        doInit(v, initArgs(2, s0, s0, s1, Signer.ids(3)));
        assertEq(v.status(), B3TypesV2.STATUS_LIVE);
        assertEq(v.currentSetHash(), s0.setHash);
        assertEq(v.nextSetHash(), s1.setHash);
        assertEq(uint256(v.lastCertificateTime()), 0);
        assertFalse(v.depositsOpen());
    }

    function test_StartCertificate_AtEpochK2() public {
        Signer.TestSet memory s2 = lineageSet(2);
        Signer.TestSet memory s3 = lineageSet(3);
        B3FinalityVerifierV2 v = deploy(START_CERT, 2, s2.setHash);
        // Set'3 (the successor) cannot start the lineage at epoch 2.
        expectInitRevert(v, initArgs(4, s3, s2, s3, Signer.none()), B3FinalityVerifierV2.BadSigningSet.selector);
        doInit(v, initArgs(4, s2, s2, s3, Signer.none()));
        assertEq(uint256(v.currentEpoch()), 2);
        assertEq(v.setHashByEpoch(2), s2.setHash);
    }

    function test_StartCertificate_ConstructorRefusesBootstrapHash() public {
        B3FinalityVerifierV2.Config memory c = config(START_CERT, 0, genesisSetHash, profileR());
        c.bootstrapSetHash = bootstrapSetHash;
        vm.expectRevert(B3FinalityVerifierV2.BadBootstrapSet.selector);
        new B3FinalityVerifierV2(c, bootstrapHeader, defaultWatchers());
        // The header argument is ignored in this mode.
        c.bootstrapSetHash = bytes32(0);
        B3Types.SetHeader memory empty;
        B3FinalityVerifierV2 v = new B3FinalityVerifierV2(c, empty, defaultWatchers());
        assertEq(v.BOOTSTRAP_SET_HASH(), bytes32(0));
    }

    // ================================================================ cancel

    function test_WatcherCancelsBeforeInitialize() public {
        Signer.TestSet memory s0 = lineageSet(0);
        Signer.TestSet memory s1 = lineageSet(1);
        B3FinalityVerifierV2 v = deploy(K_OF_N, 0, s0.setHash);
        assertTrue(v.isWatcher(WATCHER_A) && v.isWatcher(WATCHER_B));
        assertFalse(v.isWatcher(STRANGER));
        assertFalse(v.isWatcher(address(0)));

        vm.prank(STRANGER);
        vm.expectRevert(B3FinalityVerifierV2.NotWatcher.selector);
        v.cancel();

        vm.expectEmit(address(v));
        emit B3FinalityVerifierV2.Cancelled(WATCHER_B);
        vm.prank(WATCHER_B);
        v.cancel();
        assertEq(v.status(), B3TypesV2.STATUS_CANCELLED);

        Init memory a = initArgs(2, bootstrapKeys(), s0, s1, Signer.ids(3));
        expectInitRevert(v, a, B3FinalityVerifierV2.VerifierCancelled.selector);
        vm.prank(WATCHER_A);
        vm.expectRevert(B3FinalityVerifierV2.CancelClosed.selector);
        v.cancel();
    }

    function test_WatcherCancelsDuringReview() public {
        (B3FinalityVerifierV2 v, Signer.TestSet memory s0, Signer.TestSet memory s1) = liveAt0(2);
        submit(v, certify(CANONICAL, V_INDEX, 2, s0, s1));

        vm.prank(STRANGER);
        vm.expectRevert(B3FinalityVerifierV2.NotWatcher.selector);
        v.cancel();

        vm.warp(T0 + REVIEW - 1);
        vm.prank(WATCHER_A);
        v.cancel();
        assertEq(v.status(), B3TypesV2.STATUS_CANCELLED);
        assertFalse(v.depositsOpen());

        // A cancelled verifier refuses certificates, evidence and a second cancel.
        Cert memory c = certify(CANONICAL, V_INDEX + 1, V_INDEX, s0, s1);
        expectSubmitRevert(v, c, B3FinalityVerifierV2.NotLive.selector);
        vm.expectRevert(B3FinalityVerifierV2.NotLive.selector);
        reportConflict(v, c, 0, bytes32(0));
        vm.prank(WATCHER_B);
        vm.expectRevert(B3FinalityVerifierV2.CancelClosed.selector);
        v.cancel();
        // Accepted roots stay readable for the vault (C-1).
        assertEq(uint256(v.acceptedRootHeight(withdrawalRootAt(CANONICAL, V_INDEX))), V);
    }

    function test_CancelAfterReviewWindowReverts() public {
        (B3FinalityVerifierV2 v,,) = liveAt0(2);
        vm.warp(T0 + REVIEW);
        vm.prank(WATCHER_A);
        vm.expectRevert(B3FinalityVerifierV2.CancelClosed.selector);
        v.cancel();
        assertEq(v.status(), B3TypesV2.STATUS_LIVE);
    }

    function test_NoWatchersMeansNoCancel() public {
        B3FinalityVerifierV2 v =
            new B3FinalityVerifierV2(config(K_OF_N, 0, genesisSetHash, profileR()), bootstrapHeader, new address[](0));
        assertEq(v.WATCHER_COUNT(), 0);
        assertEq(v.watchers().length, 0);
        vm.prank(address(0));
        vm.expectRevert(B3FinalityVerifierV2.NotWatcher.selector);
        v.cancel();
    }

    // ============================================================== helpers

    function _expectBadBootstrap(B3FinalityVerifierV2.Config memory c, B3Types.SetHeader memory header) private {
        c.bootstrapSetHash = B3Types.hashSetHeader(header);
        vm.expectRevert(B3FinalityVerifierV2.BadBootstrapSet.selector);
        new B3FinalityVerifierV2(c, header, defaultWatchers());
    }

    function _root(bytes32[] memory leaves) private pure returns (bytes32) {
        return MerkleTreeV2.root(leaves, leaves.length, MerkleTreeV2.HISTORY_DEPTH);
    }

    function _path(bytes32[] memory leaves, uint256 index) private pure returns (bytes32[32] memory) {
        return MerkleTreeV2.path32(leaves, leaves.length, index);
    }
}
