// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {B3Types} from "../IB3FinalityProver.sol";
import {B3TypesV2} from "./B3TypesV2.sol";
import {IBlsDigestProver} from "./IBlsDigestProver.sol";
import {IB3FinalityVerifierV2} from "./IB3FinalityVerifierV2.sol";

/// Permissionless Ethereum mirror of B3's fresh finality lineage, bridge V2
/// (doc/design/b3-bridge-v2-contracts-spec.md §3-§5).
///
/// No owner, admin, pause key, proxy, rescue key or fee switch. The only
/// privileged acts are the one-time bootstrap and a watcher `cancel` before
/// deposits can open:
///
/// - Bootstrap (spec §5). In the default K_OF_N mode four pinned bootstrap
///   keys sign one recent epoch-k statement 3-of-4 before INIT_DEADLINE. In
///   the START_CERTIFICATE alternative the pinned epoch-k set itself signs it
///   and no bootstrap keys exist. Either way the statement must lie strictly
///   between GENESIS_HEIGHT and BRIDGE_ACTIVATION_HEIGHT with a zero
///   withdrawal root and zero flags (open issue OI-1, architecture §20), so
///   the bootstrap can never install a claimable root; it sets only the
///   latest checkpoint and history (the latest/history part of §3.3 step 11).
///   Attestation scope (K_OF_N): the bootstrap keys sign digestV2(fb) only, as
///   spec §5 step 3 requires. That binds the lineage (so GENESIS_SET_HASH),
///   epoch k and the epoch-(k+1) successor named by fb, but not
///   START_SET_HASH when k > 0, and not this deployment's address or chain id.
///   The epoch-k start set, which certifies until the first handover, is
///   therefore pin-only: reviewers must check it against B3's
///   getfinalitylineage, and a published attestation can initialize another
///   deployment with the same lineage, bootstrap header and START_EPOCH.
/// - Certificates (spec §3.3). Anyone relays a digest-V2 certificate signed by
///   the dual quorum of the current set, or of the pinned next set on an epoch
///   handover. Each must extend the last accepted checkpoint in B3's
///   checkpoint history tree (ancestry, spec §2.3), so relayers may skip
///   certificates. Roots certified by a bridge-qualified set at or above
///   BRIDGE_ACTIVATION_HEIGHT stay claimable for ever (acceptedRootHeight,
///   written once per root).
/// - Conflict halt (spec §3.4). A certificate of the current or next set that
///   contradicts the stored history halts the verifier for good. Releases
///   against roots accepted before the halt continue in the vault (C-1).
/// - Windows (spec §4). A set expires MAX_EPOCH_LAG after the last rotation;
///   handovers are at least MIN_EPOCH_DURATION apart; same-epoch certificates
///   never renew the rotation clock. On every chain id but 31337 the review
///   window is at least 7 days and a K_OF_N deployment names a watcher.
///
/// Invariant: nextSetHash != 0 whenever status == LIVE. initialize and every
/// rotation pin fb.validatorSetHash, which step 5 requires to be non-zero,
/// and nothing clears it. Spec §3.3 steps 2 and 10 and §3.5 test for a zero
/// nextSetHash; those tests are always settled while LIVE, so they are not
/// repeated here.
///
/// Storage keeps set hashes only; callers pass the signing header in calldata
/// (C-8). The slot layout below is normative (spec §3.2) because B3 proves
/// these slots by EIP-1186; do not reorder state variables or add inheritance
/// with storage.
contract B3FinalityVerifierV2 is IB3FinalityVerifierV2 {
    enum BootstrapMode {
        K_OF_N,
        START_CERTIFICATE
    }

    struct Config {
        bytes32 chainDomain;
        uint64 genesisHeight; // R_f - 1: history leaf 0
        bytes32 genesisBlockHash;
        bytes32 genesisSetHash; // setHash(Set'_0)
        bytes32 lineageId;
        uint64 startEpoch; // k
        bytes32 startSetHash;
        BootstrapMode bootstrapMode;
        bytes32 bootstrapSetHash; // K_OF_N: hash of the 4-key header; START_CERTIFICATE: zero
        IBlsDigestProver prover;
        bytes32 proverCodeHash;
        uint64 bridgeActivationHeight; // V
        uint32 minBridgeValidators;
        uint32 maxBridgeValidators;
        uint64 minBridgeTotalWeight;
        uint256 minEpochDuration;
        uint256 maxEpochLag;
        uint256 depositCloseAfter;
        uint256 maxCertificateAge;
        uint256 initDeadline; // absolute timestamp
        uint256 reviewWindow;
    }

    /// Slot 0. Bit offsets 0 / 64 / 104 / 144 / 208 / 216.
    struct Liveness {
        uint64 currentEpoch;
        uint40 lastRotationTime;
        uint40 lastCertificateTime;
        uint64 latestBridgeHeight;
        uint8 status;
        bool depositsClosed;
    }

    /// Slot 3. Bit offsets 0 / 32 / 96 / 128.
    struct SetSummary {
        uint32 currentCount;
        uint64 currentWeight;
        uint32 nextCount;
        uint64 nextWeight;
    }

    /// Slot 4. Bit offsets 0 / 64 / 128.
    struct Latest {
        uint64 height;
        uint64 index;
        uint40 initTime;
    }

    /// Chain id on which the relaxed regtest window orderings apply (OI-10, C-12).
    uint256 public constant REGTEST_CHAIN_ID = 31_337;
    uint256 public constant MAX_WATCHERS = 5;
    /// Window rules on every other chain id (spec §4).
    uint256 public constant MIN_MAINNET_EPOCH_DURATION = 3_600;
    uint256 public constant MAINNET_LAG_MARGIN = 7 days;
    /// REVIEW_WINDOW floor on every chain id but 31337 (spec §3.1 default).
    uint256 public constant MIN_MAINNET_REVIEW_WINDOW = 7 days;
    /// Every window is at most MAX_EPOCH_LAG, which is bounded so timestamp
    /// sums never overflow and fit the uint40 clocks of slot 0.
    uint256 public constant MAX_WINDOW = type(uint40).max;
    uint32 public constant MIN_BRIDGE_VALIDATORS_FLOOR = 4;
    /// K_OF_N bootstrap header: equal synthetic weights make both prover
    /// quorums exactly 3-of-4 (never 2-of-3: any two quorums share a signer).
    uint32 public constant BOOTSTRAP_MEMBERS = 4;
    uint64 public constant BOOTSTRAP_TOTAL_WEIGHT = 4;
    uint64 public constant BOOTSTRAP_QUORUM_WEIGHT = 3;

    /// reportConflict forms (spec §3.4).
    uint8 public constant CONFLICT_SAME_INDEX = 1;
    uint8 public constant CONFLICT_LATER_INDEX = 2;
    uint8 public constant CONFLICT_EARLIER_INDEX = 3;

    bytes32 public immutable CHAIN_DOMAIN;
    uint64 public immutable GENESIS_HEIGHT;
    bytes32 public immutable GENESIS_BLOCK_HASH;
    bytes32 public immutable GENESIS_SET_HASH;
    bytes32 public immutable LINEAGE_ID;
    uint64 public immutable START_EPOCH;
    bytes32 public immutable START_SET_HASH;
    BootstrapMode public immutable BOOTSTRAP_MODE;
    bytes32 public immutable BOOTSTRAP_SET_HASH;
    IBlsDigestProver public immutable PROVER;
    bytes32 public immutable PROVER_CODE_HASH;
    uint64 public immutable BRIDGE_ACTIVATION_HEIGHT;
    uint32 public immutable MIN_BRIDGE_VALIDATORS;
    uint32 public immutable MAX_BRIDGE_VALIDATORS;
    uint64 public immutable MIN_BRIDGE_TOTAL_WEIGHT;
    uint256 public immutable MIN_EPOCH_DURATION;
    uint256 public immutable MAX_EPOCH_LAG;
    uint256 public immutable DEPOSIT_CLOSE_AFTER;
    uint256 public immutable MAX_CERTIFICATE_AGE;
    uint256 public immutable INIT_DEADLINE;
    uint256 public immutable REVIEW_WINDOW;
    address public immutable WATCHER_0;
    address public immutable WATCHER_1;
    address public immutable WATCHER_2;
    address public immutable WATCHER_3;
    address public immutable WATCHER_4;
    uint256 public immutable WATCHER_COUNT;

    // ------------------------------------------------ normative slots (§3.2)
    Liveness private _liveness; // slot 0
    bytes32 public currentSetHash; // slot 1
    bytes32 public nextSetHash; // slot 2 (zero only before initialize)
    SetSummary private _summary; // slot 3
    Latest private _latest; // slot 4
    bytes32 public latestBlockHash; // slot 5
    bytes32 public latestCertId; // slot 6
    bytes32 public latestHistoryRoot; // slot 7
    bytes32 public latestBridgeRoot; // slot 8
    mapping(bytes32 root => uint64 height) public acceptedRootHeight; // slot 9
    mapping(uint64 epoch => bytes32 setHash) public setHashByEpoch; // slot 10

    event Initialized(
        bytes32 indexed certId, uint64 indexed epoch, uint64 indexed height, bytes32 nextSetHash, bytes signerBitmap
    );
    event Finalized(
        bytes32 indexed certId, uint64 indexed epoch, uint64 indexed height, bytes32 withdrawalRoot, bytes signerBitmap
    );
    event DepositsClosedForGood(bytes32 indexed certId, uint64 indexed height);
    event ConflictReported(
        bytes32 indexed certId, uint64 indexed checkpointIndex, uint8 form, address indexed reporter
    );
    event Cancelled(address indexed watcher);

    // Constructor.
    error BadConfig();
    error BadWindows();
    error BadBootstrapSet();
    error BadProver();
    error BadWatchers();
    // initialize.
    error AlreadyInitialized();
    error VerifierCancelled();
    error InitDeadlinePassed();
    error BadStartHeight();
    error BadBridgeFlags();
    error BadStartSet();
    // Certificates and evidence.
    error NotLive();
    error EpochNotAccepted();
    error SetExpired();
    error EpochTooSoon();
    error BadLineage();
    error BadCheckpointIndex();
    error NotMonotone();
    error BadAncestry();
    error BadFinalizedBlock();
    error PrematureWithdrawalRoot();
    error MissingWithdrawalRoot();
    error BadSuccessor();
    error BadSigningSet();
    error SuccessorMismatch();
    error ProofRejected();
    error NoConflict();
    // cancel.
    error NotWatcher();
    error CancelClosed();

    constructor(Config memory cfg, B3Types.SetHeader memory bootstrapSet, address[] memory watcherAddresses) {
        if (
            cfg.chainDomain == bytes32(0) || cfg.genesisHeight == 0 || cfg.genesisBlockHash == bytes32(0)
                || cfg.genesisSetHash == bytes32(0)
                || cfg.lineageId
                    != B3TypesV2.lineageId(cfg.chainDomain, cfg.genesisHeight, cfg.genesisBlockHash, cfg.genesisSetHash)
                || cfg.startSetHash == bytes32(0) || (cfg.startEpoch == 0 && cfg.startSetHash != cfg.genesisSetHash)
                || cfg.bridgeActivationHeight <= cfg.genesisHeight
                || cfg.minBridgeValidators < MIN_BRIDGE_VALIDATORS_FLOOR
                || cfg.maxBridgeValidators < cfg.minBridgeValidators
                || cfg.maxBridgeValidators > B3Types.MAX_PROVEN_BRIDGE_VALIDATORS || cfg.minBridgeTotalWeight == 0
        ) revert BadConfig();

        if (!_windowsValid(cfg)) revert BadWindows();

        if (cfg.bootstrapMode == BootstrapMode.K_OF_N) {
            if (
                cfg.bootstrapSetHash == bytes32(0) || bootstrapSet.epoch != 0
                    || bootstrapSet.validatorCount != BOOTSTRAP_MEMBERS
                    || bootstrapSet.totalWeight != BOOTSTRAP_TOTAL_WEIGHT
                    || bootstrapSet.quorumWeight != BOOTSTRAP_QUORUM_WEIGHT || !B3TypesV2.headerShapeValid(bootstrapSet)
                    || B3Types.hashSetHeader(bootstrapSet) != cfg.bootstrapSetHash
            ) revert BadBootstrapSet();
        } else if (cfg.bootstrapSetHash != bytes32(0)) {
            // START_CERTIFICATE: no bootstrap keys exist; the header argument is ignored.
            revert BadBootstrapSet();
        }

        address prover = address(cfg.prover);
        if (
            prover == address(0) || prover.code.length == 0 || cfg.proverCodeHash == bytes32(0)
                || prover.codehash != cfg.proverCodeHash
        ) revert BadProver();

        uint256 watcherCount = watcherAddresses.length;
        if (watcherCount > MAX_WATCHERS) revert BadWatchers();
        // K_OF_N trusts the review window plus a watcher cancel (spec §5 step 4).
        if (block.chainid != REGTEST_CHAIN_ID && cfg.bootstrapMode == BootstrapMode.K_OF_N && watcherCount == 0) {
            revert BadWatchers();
        }
        for (uint256 i = 0; i < watcherCount; ++i) {
            if (watcherAddresses[i] == address(0)) revert BadWatchers();
            for (uint256 j = 0; j < i; ++j) {
                if (watcherAddresses[j] == watcherAddresses[i]) revert BadWatchers();
            }
        }

        CHAIN_DOMAIN = cfg.chainDomain;
        GENESIS_HEIGHT = cfg.genesisHeight;
        GENESIS_BLOCK_HASH = cfg.genesisBlockHash;
        GENESIS_SET_HASH = cfg.genesisSetHash;
        LINEAGE_ID = cfg.lineageId;
        START_EPOCH = cfg.startEpoch;
        START_SET_HASH = cfg.startSetHash;
        BOOTSTRAP_MODE = cfg.bootstrapMode;
        BOOTSTRAP_SET_HASH = cfg.bootstrapSetHash;
        PROVER = cfg.prover;
        PROVER_CODE_HASH = cfg.proverCodeHash;
        BRIDGE_ACTIVATION_HEIGHT = cfg.bridgeActivationHeight;
        MIN_BRIDGE_VALIDATORS = cfg.minBridgeValidators;
        MAX_BRIDGE_VALIDATORS = cfg.maxBridgeValidators;
        MIN_BRIDGE_TOTAL_WEIGHT = cfg.minBridgeTotalWeight;
        MIN_EPOCH_DURATION = cfg.minEpochDuration;
        MAX_EPOCH_LAG = cfg.maxEpochLag;
        DEPOSIT_CLOSE_AFTER = cfg.depositCloseAfter;
        MAX_CERTIFICATE_AGE = cfg.maxCertificateAge;
        INIT_DEADLINE = cfg.initDeadline;
        REVIEW_WINDOW = cfg.reviewWindow;
        WATCHER_0 = watcherCount > 0 ? watcherAddresses[0] : address(0);
        WATCHER_1 = watcherCount > 1 ? watcherAddresses[1] : address(0);
        WATCHER_2 = watcherCount > 2 ? watcherAddresses[2] : address(0);
        WATCHER_3 = watcherCount > 3 ? watcherAddresses[3] : address(0);
        WATCHER_4 = watcherCount > 4 ? watcherAddresses[4] : address(0);
        WATCHER_COUNT = watcherCount;
    }

    // ================================================================ bootstrap

    /// One-time bootstrap (spec §5 step 3). `fb` is B3's canonical epoch-k
    /// statement below V; `signingSet` is the pinned bootstrap header (K_OF_N)
    /// or the start set itself (START_CERTIFICATE); `startSet` hashes to
    /// START_SET_HASH; `successor` is the epoch-(k+1) set named by fb; and
    /// `leaf0Path` places historyLeaf(GENESIS_HEIGHT, GENESIS_BLOCK_HASH) at
    /// index 0 under fb.checkpointHistoryRoot. Deposits still wait for the
    /// review window and an ordinary certificate (lastCertificateTime stays 0).
    /// In K_OF_N mode the bootstrap keys attest digestV2(fb) alone; see the
    /// contract comment for what that does not bind (START_SET_HASH for
    /// k > 0, the deployment address).
    function initialize(
        B3TypesV2.FinalizedBlockV2 calldata fb,
        B3Types.SetHeader calldata signingSet,
        B3Types.SetHeader calldata startSet,
        B3Types.SetHeader calldata successor,
        bytes32[32] calldata leaf0Path,
        bytes calldata proof
    ) external {
        uint8 currentStatus = _liveness.status;
        if (currentStatus == B3TypesV2.STATUS_CANCELLED) revert VerifierCancelled();
        if (currentStatus != B3TypesV2.STATUS_UNINITIALIZED) revert AlreadyInitialized();
        if (block.timestamp > INIT_DEADLINE) revert InitDeadlinePassed();

        if (fb.lineageId != LINEAGE_ID) revert BadLineage();
        if (fb.checkpointIndex == 0 || fb.checkpointIndex >= B3TypesV2.MAX_INDEX) revert BadCheckpointIndex();
        if (fb.epoch != START_EPOCH) revert EpochNotAccepted();
        // OI-1: the bootstrap statement can never carry a withdrawal root.
        if (fb.height <= GENESIS_HEIGHT || fb.height >= BRIDGE_ACTIVATION_HEIGHT) revert BadStartHeight();
        if (fb.withdrawalRoot != bytes32(0)) revert PrematureWithdrawalRoot();
        if (fb.bridgeFlags != 0) revert BadBridgeFlags();
        if (fb.blockHash == bytes32(0) || fb.validatorSetHash == bytes32(0)) revert BadFinalizedBlock();

        if (
            startSet.epoch != START_EPOCH || !B3TypesV2.headerShapeValid(startSet)
                || B3Types.hashSetHeader(startSet) != START_SET_HASH
        ) revert BadStartSet();
        _checkSuccessor(fb, successor);

        bytes32 genesisLeaf = B3TypesV2.historyLeaf(GENESIS_HEIGHT, GENESIS_BLOCK_HASH);
        if (B3TypesV2.rootFromPath(genesisLeaf, 0, leaf0Path) != fb.checkpointHistoryRoot) revert BadAncestry();

        bytes32 signingSetHash = BOOTSTRAP_MODE == BootstrapMode.K_OF_N ? BOOTSTRAP_SET_HASH : START_SET_HASH;
        if (B3Types.hashSetHeader(signingSet) != signingSetHash) revert BadSigningSet();
        if (!PROVER.verifyDigest(B3TypesV2.digest(CHAIN_DOMAIN, fb), signingSetHash, signingSet, proof)) {
            revert ProofRejected();
        }

        uint40 now40 = _now40();
        bytes32 certId = B3TypesV2.certId(fb);
        _liveness = Liveness({
            currentEpoch: START_EPOCH,
            lastRotationTime: now40,
            lastCertificateTime: 0,
            latestBridgeHeight: 0,
            status: B3TypesV2.STATUS_LIVE,
            depositsClosed: false
        });
        currentSetHash = START_SET_HASH;
        nextSetHash = fb.validatorSetHash;
        _summary = SetSummary({
            currentCount: startSet.validatorCount,
            currentWeight: startSet.totalWeight,
            nextCount: successor.validatorCount,
            nextWeight: successor.totalWeight
        });
        _latest = Latest({height: fb.height, index: fb.checkpointIndex, initTime: now40});
        latestBlockHash = fb.blockHash;
        latestCertId = certId;
        latestHistoryRoot = fb.checkpointHistoryRoot;
        setHashByEpoch[START_EPOCH] = START_SET_HASH;

        emit Initialized(certId, fb.epoch, fb.height, fb.validatorSetHash, _bitmapOf(proof));
    }

    // ============================================================= certificates

    /// Anyone may relay a certificate (spec §3.3 steps 1-11). No relayer gains
    /// authority: acceptance depends only on the pinned lineage, its history
    /// and the signing set's dual quorum.
    function submitCertificate(
        B3TypesV2.FinalizedBlockV2 calldata fb,
        B3Types.SetHeader calldata signingSet,
        B3Types.SetHeader calldata successor,
        bytes32[32] calldata ancestryPath,
        bytes calldata proof
    ) external {
        Liveness memory live = _liveness;
        // 1.
        if (live.status != B3TypesV2.STATUS_LIVE) revert NotLive();

        // 2. Only the current epoch, or its successor (always pinned while LIVE).
        bool transition = fb.epoch == live.currentEpoch + 1;
        if (!transition && fb.epoch != live.currentEpoch) revert EpochNotAccepted();

        // 3. One bounded lifetime per set; handovers consume real time.
        uint256 sinceRotation = block.timestamp - live.lastRotationTime;
        if (sinceRotation > MAX_EPOCH_LAG) revert SetExpired();
        if (transition && sinceRotation < MIN_EPOCH_DURATION) revert EpochTooSoon();

        // 4. Lineage and ancestry (spec §2.3).
        if (fb.lineageId != LINEAGE_ID) revert BadLineage();
        Latest memory last = _latest;
        if (fb.checkpointIndex <= last.index || fb.checkpointIndex >= B3TypesV2.MAX_INDEX) {
            revert BadCheckpointIndex();
        }
        if (fb.height <= last.height) revert NotMonotone();
        if (
            B3TypesV2.rootFromPath(B3TypesV2.historyLeaf(last.height, latestBlockHash), last.index, ancestryPath)
                != fb.checkpointHistoryRoot
        ) revert BadAncestry();

        // 5. Statement hashes; the withdrawal root is zero exactly below V.
        if (fb.blockHash == bytes32(0) || fb.validatorSetHash == bytes32(0)) revert BadFinalizedBlock();
        if (fb.height < BRIDGE_ACTIVATION_HEIGHT) {
            if (fb.withdrawalRoot != bytes32(0)) revert PrematureWithdrawalRoot();
        } else if (fb.withdrawalRoot == bytes32(0)) {
            revert MissingWithdrawalRoot();
        }

        // 6.
        _checkSuccessor(fb, successor);

        // 7.
        bytes32 next = nextSetHash;
        bytes32 signingSetHash = transition ? next : currentSetHash;
        if (B3Types.hashSetHeader(signingSet) != signingSetHash) revert BadSigningSet();

        // Step 10's same-epoch successor rule depends only on stored state and
        // fb, so it is checked before the expensive proof (same outcome).
        if (!transition && fb.validatorSetHash != next) revert SuccessorMismatch();

        // 8.
        if (!PROVER.verifyDigest(B3TypesV2.digest(CHAIN_DOMAIN, fb), signingSetHash, signingSet, proof)) {
            revert ProofRejected();
        }

        bytes32 certId = B3TypesV2.certId(fb);

        // 9. Bridge roots only from qualified sets, from V on; each root keeps
        // the first height at which it was accepted.
        if (
            fb.height >= BRIDGE_ACTIVATION_HEIGHT
                && B3TypesV2.qualified(
                    signingSet.validatorCount,
                    signingSet.totalWeight,
                    MIN_BRIDGE_VALIDATORS,
                    MAX_BRIDGE_VALIDATORS,
                    MIN_BRIDGE_TOTAL_WEIGHT
                )
        ) {
            live.latestBridgeHeight = fb.height;
            latestBridgeRoot = fb.withdrawalRoot;
            if (acceptedRootHeight[fb.withdrawalRoot] == 0) acceptedRootHeight[fb.withdrawalRoot] = fb.height;
        }

        // 10. Rotate only after the next set's proof verified, then pin the
        // successor. A same-epoch certificate named the pinned successor
        // (checked above), so it changes no set.
        uint40 now40 = _now40();
        if (transition) {
            SetSummary memory summary = _summary;
            currentSetHash = next;
            live.currentEpoch = fb.epoch;
            setHashByEpoch[fb.epoch] = next;
            live.lastRotationTime = now40;
            _summary = SetSummary({
                currentCount: summary.nextCount,
                currentWeight: summary.nextWeight,
                nextCount: successor.validatorCount,
                nextWeight: successor.totalWeight
            });
            nextSetHash = fb.validatorSetHash;
        }

        // 11. Latest checkpoint, certificate clock and the sticky deposit close.
        _latest = Latest({height: fb.height, index: fb.checkpointIndex, initTime: last.initTime});
        latestBlockHash = fb.blockHash;
        latestCertId = certId;
        latestHistoryRoot = fb.checkpointHistoryRoot;
        live.lastCertificateTime = now40;
        bool closing = (fb.bridgeFlags & B3TypesV2.FLAG_DEPOSITS_CLOSED) != 0 && !live.depositsClosed;
        if (closing) live.depositsClosed = true;
        _liveness = live;

        if (closing) emit DepositsClosedForGood(certId, fb.height);
        emit Finalized(certId, fb.epoch, fb.height, fb.withdrawalRoot, _bitmapOf(proof));
    }

    /// Halts the verifier for good on a certificate of the current or next set
    /// that contradicts the stored history (spec §3.4). `leafHeight` and
    /// `leafHash` name the history leaf L that `path` places: at latestIndex
    /// under fb.checkpointHistoryRoot when fb's index is higher, at fb's index
    /// under latestHistoryRoot when it is lower; unused for the same index.
    /// Rotated-out sets cannot halt anything. Evidence is refused once the
    /// current set has expired (MAX_EPOCH_LAG after the last rotation), as
    /// certificates are (I-4): expired keys can neither certify nor halt.
    /// Spec §3.4 states no expiry condition; this is a reported choice. An
    /// expired verifier that never halted still reads status LIVE.
    function reportConflict(
        B3TypesV2.FinalizedBlockV2 calldata fb,
        B3Types.SetHeader calldata signingSet,
        uint64 leafHeight,
        bytes32 leafHash,
        bytes32[32] calldata path,
        bytes calldata proof
    ) external {
        Liveness memory live = _liveness;
        if (live.status != B3TypesV2.STATUS_LIVE) revert NotLive();
        if (block.timestamp - live.lastRotationTime > MAX_EPOCH_LAG) revert SetExpired();
        if (fb.lineageId != LINEAGE_ID) revert BadLineage();
        uint64 index = fb.checkpointIndex;
        if (index == 0 || index >= B3TypesV2.MAX_INDEX) revert BadCheckpointIndex();

        bytes32 signingSetHash = B3Types.hashSetHeader(signingSet);
        if (signingSetHash != currentSetHash && signingSetHash != nextSetHash) revert BadSigningSet();

        Latest memory last = _latest;
        bytes32 certId = B3TypesV2.certId(fb);
        uint8 form;
        if (index == last.index) {
            if (certId == latestCertId) revert NoConflict();
            form = CONFLICT_SAME_INDEX;
        } else if (index > last.index) {
            bytes32 leaf = B3TypesV2.historyLeaf(leafHeight, leafHash);
            if (
                leaf == B3TypesV2.historyLeaf(last.height, latestBlockHash)
                    || B3TypesV2.rootFromPath(leaf, last.index, path) != fb.checkpointHistoryRoot
            ) revert NoConflict();
            form = CONFLICT_LATER_INDEX;
        } else {
            bytes32 leaf = B3TypesV2.historyLeaf(leafHeight, leafHash);
            if (
                leaf == B3TypesV2.historyLeaf(fb.height, fb.blockHash)
                    || B3TypesV2.rootFromPath(leaf, index, path) != latestHistoryRoot
            ) revert NoConflict();
            form = CONFLICT_EARLIER_INDEX;
        }

        if (!PROVER.verifyDigest(B3TypesV2.digest(CHAIN_DOMAIN, fb), signingSetHash, signingSet, proof)) {
            revert ProofRejected();
        }

        _liveness.status = B3TypesV2.STATUS_HALTED_CONFLICT;
        emit ConflictReported(certId, index, form, msg.sender);
    }

    /// A watcher may cancel before initialization or during the review window
    /// after it, before deposits can open. CANCELLED is final; it strands
    /// nothing because deposits never opened.
    function cancel() external {
        if (!isWatcher(msg.sender)) revert NotWatcher();
        uint8 currentStatus = _liveness.status;
        bool open = currentStatus == B3TypesV2.STATUS_UNINITIALIZED
            || (currentStatus == B3TypesV2.STATUS_LIVE && block.timestamp < uint256(_latest.initTime) + REVIEW_WINDOW);
        if (!open) revert CancelClosed();
        _liveness.status = B3TypesV2.STATUS_CANCELLED;
        emit Cancelled(msg.sender);
    }

    // ==================================================================== views

    /// Deposit gate (spec §3.5). Stronger than chain finality: a live verifier
    /// past review, deposits never closed by B3's flag, a recent certificate,
    /// a recent rotation, a bridge root from V on, and a qualified current and
    /// next set (the next set is always known while LIVE).
    function depositsOpen() external view returns (bool) {
        Liveness memory live = _liveness;
        if (live.status != B3TypesV2.STATUS_LIVE || live.depositsClosed || live.lastCertificateTime == 0) {
            return false;
        }
        if (block.timestamp < uint256(_latest.initTime) + REVIEW_WINDOW) return false;
        if (block.timestamp - live.lastRotationTime > DEPOSIT_CLOSE_AFTER) return false;
        if (block.timestamp - live.lastCertificateTime > MAX_CERTIFICATE_AGE) return false;
        if (live.latestBridgeHeight < BRIDGE_ACTIVATION_HEIGHT) return false;
        SetSummary memory summary = _summary;
        return
            _qualified(summary.currentCount, summary.currentWeight) && _qualified(summary.nextCount, summary.nextWeight);
    }

    function setExpiresAt() external view returns (uint256) {
        return uint256(_liveness.lastRotationTime) + MAX_EPOCH_LAG;
    }

    function depositsCloseAt() external view returns (uint256) {
        Liveness memory live = _liveness;
        uint256 byRotation = uint256(live.lastRotationTime) + DEPOSIT_CLOSE_AFTER;
        uint256 byCertificate = uint256(live.lastCertificateTime) + MAX_CERTIFICATE_AGE;
        return byRotation < byCertificate ? byRotation : byCertificate;
    }

    function status() external view returns (uint8) {
        return _liveness.status;
    }

    function currentEpoch() external view returns (uint64) {
        return _liveness.currentEpoch;
    }

    function lastRotationTime() external view returns (uint40) {
        return _liveness.lastRotationTime;
    }

    function lastCertificateTime() external view returns (uint40) {
        return _liveness.lastCertificateTime;
    }

    function latestBridgeHeight() external view returns (uint64) {
        return _liveness.latestBridgeHeight;
    }

    function depositsClosed() external view returns (bool) {
        return _liveness.depositsClosed;
    }

    function latestHeight() external view returns (uint64) {
        return _latest.height;
    }

    function latestIndex() external view returns (uint64) {
        return _latest.index;
    }

    function initTime() external view returns (uint40) {
        return _latest.initTime;
    }

    function setSummary()
        external
        view
        returns (uint32 currentCount, uint64 currentWeight, uint32 nextCount, uint64 nextWeight)
    {
        SetSummary memory summary = _summary;
        return (summary.currentCount, summary.currentWeight, summary.nextCount, summary.nextWeight);
    }

    function currentQualified() external view returns (bool) {
        return _qualified(_summary.currentCount, _summary.currentWeight);
    }

    function nextQualified() external view returns (bool) {
        return _qualified(_summary.nextCount, _summary.nextWeight);
    }

    function isWatcher(address account) public view returns (bool) {
        return account != address(0)
            && (account == WATCHER_0
                || account == WATCHER_1
                || account == WATCHER_2
                || account == WATCHER_3
                || account == WATCHER_4);
    }

    function watchers() external view returns (address[] memory list) {
        list = new address[](WATCHER_COUNT);
        address[5] memory all = [WATCHER_0, WATCHER_1, WATCHER_2, WATCHER_3, WATCHER_4];
        for (uint256 i = 0; i < list.length; ++i) {
            list[i] = all[i];
        }
    }

    /// The BLS message of `fb` under this deployment's chain domain.
    function digestV2(B3TypesV2.FinalizedBlockV2 calldata fb) external view returns (bytes32) {
        return B3TypesV2.digest(CHAIN_DOMAIN, fb);
    }

    function certIdOf(B3TypesV2.FinalizedBlockV2 calldata fb) external pure returns (bytes32) {
        return B3TypesV2.certId(fb);
    }

    function hashSetHeader(B3Types.SetHeader calldata header) external pure returns (bytes32) {
        return B3Types.hashSetHeader(header);
    }

    // ================================================================ internals

    function _checkSuccessor(B3TypesV2.FinalizedBlockV2 calldata fb, B3Types.SetHeader calldata successor)
        private
        pure
    {
        if (
            successor.epoch != fb.epoch + 1 || !B3TypesV2.headerShapeValid(successor)
                || B3Types.hashSetHeader(successor) != fb.validatorSetHash
        ) revert BadSuccessor();
    }

    function _qualified(uint32 count, uint64 weight) private view returns (bool) {
        return B3TypesV2.qualified(count, weight, MIN_BRIDGE_VALIDATORS, MAX_BRIDGE_VALIDATORS, MIN_BRIDGE_TOTAL_WEIGHT);
    }

    function _now40() private view returns (uint40) {
        // block.timestamp stays below 2^40 until the year 36812.
        // forge-lint: disable-next-line(unsafe-typecast)
        return uint40(block.timestamp);
    }

    /// Spec §4 window orderings. MAX_EPOCH_LAG bounds every other window, and
    /// is itself bounded, so no sum below can overflow.
    function _windowsValid(Config memory cfg) private view returns (bool) {
        uint256 minEpoch = cfg.minEpochDuration;
        uint256 lag = cfg.maxEpochLag;
        uint256 close = cfg.depositCloseAfter;
        uint256 age = cfg.maxCertificateAge;
        if (lag > MAX_WINDOW || age == 0 || age >= close || minEpoch == 0 || minEpoch > lag) return false;
        if (block.chainid == REGTEST_CHAIN_ID) {
            // OI-10 regtest orderings: 0 < age < close < lag, 0 < minEpoch <= lag.
            if (close >= lag) return false;
        } else {
            // Mainnet ids: minEpoch >= 1 h, lag >= close + 7 days, close > age > 0,
            // review >= 7 days.
            if (
                minEpoch < MIN_MAINNET_EPOCH_DURATION || close > lag || lag - close < MAINNET_LAG_MARGIN
                    || cfg.reviewWindow < MIN_MAINNET_REVIEW_WINDOW
            ) return false;
        }
        if (cfg.initDeadline <= block.timestamp || cfg.initDeadline - block.timestamp > lag) return false;
        return cfg.reviewWindow != 0 && cfg.reviewWindow < lag;
    }

    /// Signer bitmap of a V1-ABI witness, for the events only. Malformed data
    /// yields an empty bitmap and never bypasses the prover.
    function _bitmapOf(bytes calldata proof) private pure returns (bytes memory) {
        if (proof.length < 64) return "";
        uint256 offset;
        assembly {
            offset := calldataload(proof.offset)
        }
        if (offset > proof.length - 32) return "";
        uint256 length;
        assembly {
            length := calldataload(add(proof.offset, offset))
        }
        if (length > proof.length - offset - 32) return "";
        return proof[offset + 32:offset + 32 + length];
    }
}
