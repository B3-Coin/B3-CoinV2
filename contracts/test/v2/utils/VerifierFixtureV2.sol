// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {V2TestBase} from "../V2TestBase.sol";
import {BlsTestSignerV2 as Signer, BlsDigestProverHarnessV2} from "./BlsTestSignerV2.sol";
import {MerkleTreeV2} from "./MerkleTreeV2.sol";
import {B3Types} from "../../../src/IB3FinalityProver.sol";
import {B3TypesV2} from "../../../src/v2/B3TypesV2.sol";
import {BlsDigestProver} from "../../../src/v2/BlsDigestProver.sol";
import {B3FinalityVerifierV2} from "../../../src/v2/B3FinalityVerifierV2.sol";

/// Shared fixture for the B3FinalityVerifierV2 suites (test only).
///
/// Chain model, regtest shape (regtest plan §3; finality dependency §7):
/// GENESIS_HEIGHT G = 261 (R_f - 1), a 5-block checkpoint grid, so the
/// checkpoint at index i has height G + 5i, and BRIDGE_ACTIVATION_HEIGHT
/// V = G + 50 = 311 is checkpoint index 10. Block hashes are synthetic: a
/// branch is named by the first index at which it leaves the canonical chain
/// (CANONICAL never does), so leaves below the fork point are shared.
///
/// Sets are real BLS sets from the in-EVM signer: Set'0 and Set'1 share keys
/// (epochs 0 and 1 share one binding, regtest plan RT4) and every later epoch
/// uses a fresh seed, so Set'2, Set'3, ... share no key with any other set
/// (B-37). Lineage sets are 4 members x 250 (bridge-qualified: weight 1,000 >=
/// 900); the unqualified set and the bootstrap keys are 4 x 1.
///
/// Windows are profile R (regtest plan §3.3: B-10 scaled by 1/36) on the
/// default test chain id 31337; profileB10() is the chain-1 default.
abstract contract VerifierFixtureV2 is V2TestBase {
    // ---- chain model
    uint64 internal constant G = 261;
    uint64 internal constant INTERVAL = 5;
    uint64 internal constant V = G + 50;
    uint64 internal constant V_INDEX = (V - G) / INTERVAL;
    uint64 internal constant CANONICAL = type(uint64).max;
    /// regtest_chain_domain() (test/functional/test_framework/b3bridge.py; vectors JSON).
    bytes32 internal constant DOMAIN = 0x1a3405424a805d6b95600c5b7598af38b1f8f3ba25bdb2636500c76ad3f43879;

    // ---- profile R windows (seconds)
    uint256 internal constant MIN_EPOCH = 600;
    uint256 internal constant LAG = 72_000;
    uint256 internal constant CLOSE = 7_200;
    uint256 internal constant AGE = 2_400;
    uint256 internal constant REVIEW = 16_800;
    uint256 internal constant INIT_WINDOW = 16_800;

    // ---- B-10 windows (contracts spec §4) and the §5 defaults
    uint256 internal constant B10_MIN_EPOCH = 21_600;
    uint256 internal constant B10_LAG = 2_592_000;
    uint256 internal constant B10_CLOSE = 259_200;
    uint256 internal constant B10_AGE = 86_400;
    uint256 internal constant B10_REVIEW = 7 days;
    uint256 internal constant B10_INIT_WINDOW = 7 days;

    // ---- bridge qualification (V1 manifest values, spec §3.1)
    uint32 internal constant MIN_VALIDATORS = 4;
    uint32 internal constant MAX_VALIDATORS = 64;
    uint64 internal constant MIN_WEIGHT = 900;
    uint32 internal constant SET_SIZE = 4;
    uint64 internal constant QUALIFIED_WEIGHT = 250;

    uint256 internal constant T0 = 1_750_000_000;

    bytes32 internal constant SEED_SHARED_01 = keccak256("VerifierFixtureV2/Set'0 and Set'1");
    bytes32 internal constant SEED_BOOTSTRAP = keccak256("VerifierFixtureV2/bootstrap keys");

    address internal constant WATCHER_A = address(0xA11CE);
    address internal constant WATCHER_B = address(0xB0B);
    address internal constant STRANGER = address(0xBAD);

    struct Cert {
        B3TypesV2.FinalizedBlockV2 fb;
        B3Types.SetHeader signingSet;
        B3Types.SetHeader successor;
        bytes32[32] path;
        bytes proof;
    }

    struct Init {
        B3TypesV2.FinalizedBlockV2 fb;
        B3Types.SetHeader signingSet;
        B3Types.SetHeader startSet;
        B3Types.SetHeader successor;
        bytes32[32] leaf0Path;
        bytes proof;
    }

    struct Windows {
        uint256 minEpoch;
        uint256 lag;
        uint256 close;
        uint256 age;
        uint256 review;
        uint256 initWindow;
    }

    BlsDigestProver internal prover;
    BlsDigestProverHarnessV2 internal harness;
    bytes32 internal genesisSetHash;
    bytes32 internal lineageIdV;
    bytes32 internal bootstrapSetHash;
    B3Types.SetHeader internal bootstrapHeader;

    function setUp() public virtual {
        vm.warp(T0);
        prover = new BlsDigestProver();
        harness = new BlsDigestProverHarnessV2();
        genesisSetHash = lineageSet(0).setHash;
        lineageIdV = B3TypesV2.lineageId(DOMAIN, G, genesisHash(), genesisSetHash);
        Signer.TestSet memory keys = bootstrapKeys();
        bootstrapSetHash = keys.setHash;
        bootstrapHeader = keys.header;
    }

    // ============================================================ chain model

    function heightAt(uint64 index) internal pure returns (uint64) {
        return G + index * INTERVAL;
    }

    function hashAt(uint64 branch, uint64 index) internal pure returns (bytes32) {
        return index < branch
            ? keccak256(abi.encode("VerifierFixtureV2/canonical", index))
            : keccak256(abi.encode("VerifierFixtureV2/fork", branch, index));
    }

    function genesisHash() internal pure returns (bytes32) {
        return hashAt(CANONICAL, 0);
    }

    function leafAt(uint64 branch, uint64 index) internal pure returns (bytes32) {
        return B3TypesV2.historyLeaf(heightAt(index), hashAt(branch, index));
    }

    function historyLeaves(uint64 branch, uint64 count) internal pure returns (bytes32[] memory leaves) {
        leaves = new bytes32[](count);
        for (uint64 i = 0; i < count; ++i) {
            leaves[i] = leafAt(branch, i);
        }
    }

    /// Root over leaves 0 .. index-1: what the certificate at `index` signs.
    function historyRoot(uint64 branch, uint64 index) internal pure returns (bytes32) {
        return MerkleTreeV2.root(historyLeaves(branch, index), index, MerkleTreeV2.HISTORY_DEPTH);
    }

    /// Siblings of `position` in the tree over leaves 0 .. treeSize-1.
    function historyPath(uint64 branch, uint64 treeSize, uint64 position) internal pure returns (bytes32[32] memory) {
        return MerkleTreeV2.path32(historyLeaves(branch, treeSize), treeSize, position);
    }

    /// Synthetic withdrawal root: zero below V, non-zero from V (spec §2.4).
    function withdrawalRootAt(uint64 branch, uint64 index) internal pure returns (bytes32) {
        if (heightAt(index) < V) return bytes32(0);
        return keccak256(abi.encode("VerifierFixtureV2/withdrawal root", hashAt(branch, index)));
    }

    // =================================================================== sets

    function seedOf(uint64 epoch) internal pure returns (bytes32) {
        return epoch <= 1 ? SEED_SHARED_01 : keccak256(abi.encode("VerifierFixtureV2/fresh epoch seed", epoch));
    }

    /// Set'_epoch of the fresh lineage (qualified).
    function lineageSet(uint64 epoch) internal view returns (Signer.TestSet memory) {
        return Signer.makeUniformSet(harness, epoch, seedOf(epoch), SET_SIZE, QUALIFIED_WEIGHT);
    }

    /// A 4 x 1 set (weight 4 < 900): tracked for lineage, never authorizes roots.
    function unqualifiedSet(uint64 epoch) internal view returns (Signer.TestSet memory) {
        return
            Signer.makeUniformSet(harness, epoch, keccak256(abi.encode("VerifierFixtureV2/unqualified", epoch)), 4, 1);
    }

    /// Another qualified epoch-`epoch` set: a successor the lineage never named.
    function rogueSet(uint64 epoch) internal view returns (Signer.TestSet memory) {
        return Signer.makeUniformSet(
            harness, epoch, keccak256(abi.encode("VerifierFixtureV2/rogue", epoch)), SET_SIZE, QUALIFIED_WEIGHT
        );
    }

    /// The four K_OF_N bootstrap keys: epoch 0, 4 x 1, quorum 3.
    function bootstrapKeys() internal view returns (Signer.TestSet memory) {
        return Signer.makeUniformSet(harness, 0, SEED_BOOTSTRAP, 4, 1);
    }

    function sharesKey(Signer.TestSet memory a, Signer.TestSet memory b) internal pure returns (bool) {
        for (uint256 i = 0; i < a.members.length; ++i) {
            for (uint256 j = 0; j < b.members.length; ++j) {
                if (keccak256(a.members[i].pk48) == keccak256(b.members[j].pk48)) return true;
            }
        }
        return false;
    }

    // ============================================================= statements

    /// B3's canonical statement shape at `index` on `branch`, signed by an
    /// epoch-`epoch` set and naming `successorHash`.
    function statement(uint64 branch, uint64 index, uint64 epoch, bytes32 successorHash)
        internal
        view
        returns (B3TypesV2.FinalizedBlockV2 memory)
    {
        return B3TypesV2.FinalizedBlockV2({
            lineageId: lineageIdV,
            height: heightAt(index),
            blockHash: hashAt(branch, index),
            withdrawalRoot: withdrawalRootAt(branch, index),
            validatorSetHash: successorHash,
            epoch: epoch,
            checkpointIndex: index,
            checkpointHistoryRoot: historyRoot(branch, index),
            bridgeFlags: 0
        });
    }

    /// Ancestry path for a certificate at `index` on `branch` after the one at `prevIndex`.
    function ancestry(uint64 branch, uint64 index, uint64 prevIndex) internal pure returns (bytes32[32] memory) {
        return historyPath(branch, index, prevIndex);
    }

    function digestOf(B3TypesV2.FinalizedBlockV2 memory fb) internal pure returns (bytes32) {
        return B3TypesV2.digest(DOMAIN, fb);
    }

    function signFb(Signer.TestSet memory signer, B3TypesV2.FinalizedBlockV2 memory fb, uint32[] memory absent)
        internal
        view
        returns (bytes memory)
    {
        return Signer.sign(harness, signer, digestOf(fb), absent);
    }

    function makeCert(
        Signer.TestSet memory signer,
        B3TypesV2.FinalizedBlockV2 memory fb,
        Signer.TestSet memory successor,
        bytes32[32] memory path
    ) internal view returns (Cert memory c) {
        // Copies, so a test can alter a header without changing the set.
        c.fb = fb;
        c.signingSet = Signer.copyHeader(signer.header);
        c.successor = Signer.copyHeader(successor.header);
        c.path = path;
        c.proof = signFb(signer, fb, Signer.ids(SET_SIZE - 1));
    }

    /// Certificate at `index` on `branch` after `prevIndex`, signed 3-of-4 by
    /// `signer` (statement epoch = signer's epoch) and naming `successor`.
    function certify(
        uint64 branch,
        uint64 index,
        uint64 prevIndex,
        Signer.TestSet memory signer,
        Signer.TestSet memory successor
    ) internal view returns (Cert memory) {
        return makeCert(
            signer,
            statement(branch, index, signer.header.epoch, successor.setHash),
            successor,
            ancestry(branch, index, prevIndex)
        );
    }

    /// Re-signs after a test changed c.fb (all four sign).
    function resign(Cert memory c, Signer.TestSet memory signer) internal view {
        c.proof = signFb(signer, c.fb, Signer.none());
    }

    function submit(B3FinalityVerifierV2 v, Cert memory c) internal {
        v.submitCertificate(c.fb, c.signingSet, c.successor, c.path, c.proof);
    }

    /// Arguments are built before expectRevert, so the signer's own external
    /// calls can never be mistaken for the call expected to revert.
    function expectSubmitRevert(B3FinalityVerifierV2 v, Cert memory c, bytes4 selector) internal {
        vm.expectRevert(selector);
        submit(v, c);
    }

    function reportConflict(B3FinalityVerifierV2 v, Cert memory c, uint64 leafHeight, bytes32 leafHash) internal {
        v.reportConflict(c.fb, c.signingSet, leafHeight, leafHash, c.path, c.proof);
    }

    function expectConflictRevert(
        B3FinalityVerifierV2 v,
        Cert memory c,
        uint64 leafHeight,
        bytes32 leafHash,
        bytes4 selector
    ) internal {
        vm.expectRevert(selector);
        reportConflict(v, c, leafHeight, leafHash);
    }

    // ============================================================== bootstrap

    /// The message `v.initialize` verifies for `fb` on the current chain id:
    /// under K_OF_N the OI-12 bootstrap digest bound to v's address, chain id
    /// and START_SET_HASH; under START_CERTIFICATE digest V2.
    function initMessage(B3FinalityVerifierV2 v, B3TypesV2.FinalizedBlockV2 memory fb) internal view returns (bytes32) {
        if (v.BOOTSTRAP_MODE() == B3FinalityVerifierV2.BootstrapMode.K_OF_N) {
            return B3TypesV2.bootstrapDigest(v.CHAIN_DOMAIN(), block.chainid, address(v), v.START_SET_HASH(), fb);
        }
        return B3TypesV2.digest(v.CHAIN_DOMAIN(), fb);
    }

    /// Signs initMessage(v, fb); used again after a test changes a statement.
    function signInit(
        B3FinalityVerifierV2 v,
        Signer.TestSet memory signer,
        B3TypesV2.FinalizedBlockV2 memory fb,
        uint32[] memory absent
    ) internal view returns (bytes memory) {
        return Signer.sign(harness, signer, initMessage(v, fb), absent);
    }

    /// Low-level K_OF_N attestation under DOMAIN for any verifier address,
    /// chain id and start set hash, for the OI-12 replay cases.
    function signBootstrap(
        Signer.TestSet memory signer,
        address verifier,
        uint256 chainId,
        bytes32 startSetHash,
        B3TypesV2.FinalizedBlockV2 memory fb,
        uint32[] memory absent
    ) internal view returns (bytes memory) {
        bytes32 message = B3TypesV2.bootstrapDigest(DOMAIN, chainId, verifier, startSetHash, fb);
        return Signer.sign(harness, signer, message, absent);
    }

    /// Bootstrap arguments for `v`: the canonical epoch-k statement at `index`
    /// (below V) signed over initMessage(v, fb) by `signer` with `absent`
    /// members missing.
    function initArgs(
        B3FinalityVerifierV2 v,
        uint64 index,
        Signer.TestSet memory signer,
        Signer.TestSet memory startSet,
        Signer.TestSet memory successor,
        uint32[] memory absent
    ) internal view returns (Init memory a) {
        a.fb = statement(CANONICAL, index, startSet.header.epoch, successor.setHash);
        a.signingSet = Signer.copyHeader(signer.header);
        a.startSet = Signer.copyHeader(startSet.header);
        a.successor = Signer.copyHeader(successor.header);
        a.leaf0Path = historyPath(CANONICAL, index, 0);
        a.proof = signInit(v, signer, a.fb, absent);
    }

    function doInit(B3FinalityVerifierV2 v, Init memory a) internal {
        v.initialize(a.fb, a.signingSet, a.startSet, a.successor, a.leaf0Path, a.proof);
    }

    function expectInitRevert(B3FinalityVerifierV2 v, Init memory a, bytes4 selector) internal {
        vm.expectRevert(selector);
        doInit(v, a);
    }

    // ============================================================= deployment

    function profileR() internal pure returns (Windows memory) {
        return Windows(MIN_EPOCH, LAG, CLOSE, AGE, REVIEW, INIT_WINDOW);
    }

    function profileB10() internal pure returns (Windows memory) {
        return Windows(B10_MIN_EPOCH, B10_LAG, B10_CLOSE, B10_AGE, B10_REVIEW, B10_INIT_WINDOW);
    }

    function config(B3FinalityVerifierV2.BootstrapMode mode, uint64 k, bytes32 startSetHash, Windows memory w)
        internal
        view
        returns (B3FinalityVerifierV2.Config memory c)
    {
        c.chainDomain = DOMAIN;
        c.genesisHeight = G;
        c.genesisBlockHash = genesisHash();
        c.genesisSetHash = genesisSetHash;
        c.lineageId = lineageIdV;
        c.startEpoch = k;
        c.startSetHash = startSetHash;
        c.bootstrapMode = mode;
        c.bootstrapSetHash = mode == B3FinalityVerifierV2.BootstrapMode.K_OF_N ? bootstrapSetHash : bytes32(0);
        c.prover = prover;
        c.proverCodeHash = address(prover).codehash;
        c.bridgeActivationHeight = V;
        c.minBridgeValidators = MIN_VALIDATORS;
        c.maxBridgeValidators = MAX_VALIDATORS;
        c.minBridgeTotalWeight = MIN_WEIGHT;
        c.minEpochDuration = w.minEpoch;
        c.maxEpochLag = w.lag;
        c.depositCloseAfter = w.close;
        c.maxCertificateAge = w.age;
        c.initDeadline = block.timestamp + w.initWindow;
        c.reviewWindow = w.review;
    }

    function defaultWatchers() internal pure returns (address[] memory list) {
        list = new address[](2);
        list[0] = WATCHER_A;
        list[1] = WATCHER_B;
    }

    function deployWith(B3FinalityVerifierV2.Config memory c) internal returns (B3FinalityVerifierV2) {
        return new B3FinalityVerifierV2(c, bootstrapHeader, defaultWatchers());
    }

    /// Profile-R deployment starting at epoch k with Set'_k (`startSetHash`).
    function deploy(B3FinalityVerifierV2.BootstrapMode mode, uint64 k, bytes32 startSetHash)
        internal
        returns (B3FinalityVerifierV2)
    {
        return deployWith(config(mode, k, startSetHash, profileR()));
    }

    /// K_OF_N at epoch 0, bootstrapped 3-of-4 at checkpoint `index` naming
    /// Set'1. Returns the live verifier, Set'0 and Set'1.
    function liveAt0(uint64 index)
        internal
        returns (B3FinalityVerifierV2 v, Signer.TestSet memory s0, Signer.TestSet memory s1)
    {
        s0 = lineageSet(0);
        s1 = lineageSet(1);
        v = deploy(B3FinalityVerifierV2.BootstrapMode.K_OF_N, 0, s0.setHash);
        doInit(v, initArgs(v, index, bootstrapKeys(), s0, s1, Signer.ids(3)));
    }
}
