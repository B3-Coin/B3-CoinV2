// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {B3Types, IB3FinalityProver} from "./IB3FinalityProver.sol";

/// Permissionless Ethereum mirror of B3's stake-and-headcount finality lineage.
///
/// This contract deliberately has no owner, pause key, upgrade proxy, or
/// emergency signer. Authority starts with one immutable four-key bootstrap
/// committee. Any three of those keys may attest the public Set_0 snapshot
/// produced by B3 at M-1 exactly once. Authority then advances only when the
/// normal B3 stake/BLS quorum signs its successor; the bootstrap committee has
/// no callable path after initialization.
///
/// Bootstrap is fail-closed and time-bounded. The four-key header, proof
/// backend runtime, deadline, minimum bridge validator count, and minimum
/// bridge total staked weight are all pinned at deployment. A small Set_0 may
/// still be tracked for lineage continuity; it cannot authorize withdrawal
/// roots until both the current and known successor sets satisfy the bridge
/// count and weight thresholds.
contract B3FinalityVerifier {
    struct DeploymentConfig {
        uint64 modernStartHeight;
        uint64 bridgeActivationHeight;
        uint32 minBridgeValidators;
        uint32 maxBridgeValidators;
        uint64 minBridgeTotalWeight;
        uint256 minEpochDuration;
        uint256 maxEpochLag;
        uint256 maxCertificateAge;
        uint256 minDepositExitWindow;
        uint256 bootstrapDeadline;
    }

    bytes32 public immutable CHAIN_DOMAIN;
    bytes32 public immutable BOOTSTRAP_SET_HASH;
    bytes32 public immutable PROVER_CODE_HASH;
    uint64 public immutable MODERN_START_HEIGHT;
    uint64 public immutable BRIDGE_ACTIVATION_HEIGHT;
    uint32 public immutable MIN_BRIDGE_VALIDATORS;
    uint32 public immutable MAX_BRIDGE_VALIDATORS;
    uint64 public immutable MIN_BRIDGE_TOTAL_WEIGHT;
    uint256 public immutable MIN_EPOCH_DURATION;
    uint256 public immutable MAX_EPOCH_LAG;
    uint256 public immutable MAX_CERTIFICATE_AGE;
    uint256 public immutable MIN_DEPOSIT_EXIT_WINDOW;
    uint256 public immutable BOOTSTRAP_DEADLINE;
    IB3FinalityProver public immutable prover;

    bool public initialized;
    uint256 public GENESIS_TIME;
    bytes32 public GENESIS_SET_HASH;
    B3Types.SetHeader private _bootstrapSet;
    uint64 public currentEpoch;
    bytes32 public currentSetHash;
    B3Types.SetHeader public currentSet;
    bytes32 public nextSetHash;
    B3Types.SetHeader public nextSet;
    mapping(uint64 => bytes32) public setHashByEpoch;
    B3Types.FinalizedBlock public latest;
    /// Wall-clock time of the newest accepted certificate. It exposes live
    /// bridge-finality readiness, but same-epoch certificates MUST NOT renew
    /// the validator set's weak-subjectivity lifetime.
    uint256 public lastCertificateTime;
    /// Wall-clock time at which the current validator-set lineage first became
    /// live on Ethereum or most recently completed an epoch handover.
    uint256 public lastRotationTime;
    uint64 private _latestBridgeFinalizedHeight;
    bytes32 private _latestBridgeWithdrawalRoot;

    event Finalized(
        uint64 indexed epoch, uint64 indexed height, bytes32 blockHash, bytes32 withdrawalRoot, bytes signerBitmap
    );
    event Initialized(bytes32 indexed genesisSetHash, bytes32 indexed snapshotBlockHash, bytes signerBitmap);

    error BadBootstrap();
    error AlreadyInitialized();
    error NotInitialized();
    error BootstrapExpired();
    error BadGenesisSet();
    error BadProver();
    error EpochNotAccepted();
    error NotMonotone();
    error BadFinalizedBlock();
    error PrematureWithdrawalRoot();
    error MissingWithdrawalRoot();
    error BadSuccessor();
    error SuccessorMismatch();
    error EpochLagExceeded();
    error EpochTimeWindow();
    error ProofRejected();

    constructor(
        bytes32 chainDomain,
        B3Types.SetHeader memory bootstrapSet,
        bytes32 expectedBootstrapSetHash,
        IB3FinalityProver prover_,
        bytes32 expectedProverCodeHash,
        DeploymentConfig memory config
    ) {
        if (
            chainDomain == bytes32(0) || expectedBootstrapSetHash == bytes32(0) || expectedProverCodeHash == bytes32(0)
                || config.modernStartHeight == 0 || config.bridgeActivationHeight < config.modernStartHeight
                || config.minBridgeValidators < 4 || config.maxBridgeValidators < config.minBridgeValidators
                || config.maxBridgeValidators > B3Types.MAX_PROVEN_BRIDGE_VALIDATORS || config.minBridgeTotalWeight == 0
                || config.minEpochDuration < B3Types.MIN_PROVEN_EPOCH_DURATION
                || config.minEpochDuration > config.maxEpochLag || config.maxEpochLag == 0
                || config.maxCertificateAge == 0 || config.maxCertificateAge >= config.maxEpochLag
                || config.minDepositExitWindow == 0 || config.minDepositExitWindow >= config.maxEpochLag
                || config.bootstrapDeadline <= block.timestamp
                || config.bootstrapDeadline - block.timestamp > config.maxEpochLag
        ) revert BadBootstrap();

        // Equal synthetic weights make both prover quorums exactly 3-of-4.
        // The member root pins which four PoP-verified BLS identities those
        // weights belong to; the public deployment manifest carries the rows.
        if (
            bootstrapSet.epoch != 0 || bootstrapSet.validatorCount != 4 || bootstrapSet.totalWeight != 4
                || bootstrapSet.quorumWeight != 3 || !B3Types.headerShapeValid(bootstrapSet)
                || B3Types.hashSetHeader(bootstrapSet) != expectedBootstrapSetHash
        ) revert BadGenesisSet();

        if (
            address(prover_) == address(0) || address(prover_).code.length == 0
                || _codeHash(address(prover_)) != expectedProverCodeHash
        ) revert BadProver();

        CHAIN_DOMAIN = chainDomain;
        BOOTSTRAP_SET_HASH = expectedBootstrapSetHash;
        PROVER_CODE_HASH = expectedProverCodeHash;
        MODERN_START_HEIGHT = config.modernStartHeight;
        BRIDGE_ACTIVATION_HEIGHT = config.bridgeActivationHeight;
        MIN_BRIDGE_VALIDATORS = config.minBridgeValidators;
        MAX_BRIDGE_VALIDATORS = config.maxBridgeValidators;
        MIN_BRIDGE_TOTAL_WEIGHT = config.minBridgeTotalWeight;
        MIN_EPOCH_DURATION = config.minEpochDuration;
        MAX_EPOCH_LAG = config.maxEpochLag;
        MAX_CERTIFICATE_AGE = config.maxCertificateAge;
        MIN_DEPOSIT_EXIT_WINDOW = config.minDepositExitWindow;
        BOOTSTRAP_DEADLINE = config.bootstrapDeadline;
        prover = prover_;

        _bootstrapSet = bootstrapSet;
        latest = B3Types.FinalizedBlock({
            height: config.modernStartHeight - 1,
            blockHash: bytes32(0),
            withdrawalRoot: bytes32(0),
            validatorSetHash: bytes32(0),
            epoch: 0
        });
        lastCertificateTime = 0;
        lastRotationTime = 0;
    }

    /// One-time 3-of-4 handoff from the pinned bootstrap identities to the
    /// canonical B3 Set_0. `snapshot` uses the existing FinalizedBlock digest:
    /// height=M-1, the real B3 block hash, zero withdrawal root,
    /// validatorSetHash=hash(genesisSet), epoch=0. Reusing the ordinary prover
    /// keeps one BLS scheme, bitmap rule, member tree and witness ABI.
    function initialize(
        B3Types.FinalizedBlock calldata snapshot,
        B3Types.SetHeader calldata genesisSet,
        bytes calldata proof
    ) external {
        if (initialized) revert AlreadyInitialized();
        if (block.timestamp > BOOTSTRAP_DEADLINE) revert BootstrapExpired();
        if (
            snapshot.height != MODERN_START_HEIGHT - 1 || snapshot.blockHash == bytes32(0)
                || snapshot.withdrawalRoot != bytes32(0) || snapshot.validatorSetHash == bytes32(0)
                || snapshot.epoch != 0
        ) revert BadFinalizedBlock();

        bytes32 genesisSetHash = B3Types.hashSetHeader(genesisSet);
        if (
            genesisSet.epoch != 0 || !B3Types.headerShapeValid(genesisSet)
                || genesisSetHash != snapshot.validatorSetHash
        ) revert BadGenesisSet();
        if (!prover.verify(CHAIN_DOMAIN, snapshot, BOOTSTRAP_SET_HASH, _bootstrapSet, proof)) revert ProofRejected();

        initialized = true;
        GENESIS_TIME = block.timestamp;
        GENESIS_SET_HASH = genesisSetHash;
        currentSet = genesisSet;
        currentSetHash = genesisSetHash;
        setHashByEpoch[0] = genesisSetHash;
        latest = snapshot;
        lastCertificateTime = 0;
        lastRotationTime = block.timestamp;
        emit Initialized(genesisSetHash, snapshot.blockHash, _bitmapOf(proof));
    }

    /// Anyone may relay a certificate. No relayer gains authority: acceptance
    /// depends only on the pinned B3 lineage and its stake/BLS quorum.
    function submitCertificate(
        B3Types.FinalizedBlock calldata finalizedBlock,
        B3Types.SetHeader calldata successor,
        bytes calldata proof
    ) external {
        if (!initialized) revert NotInitialized();
        bool transition = finalizedBlock.epoch == currentEpoch + 1 && nextSetHash != bytes32(0);
        if (finalizedBlock.epoch != currentEpoch && !transition) {
            revert EpochNotAccepted();
        }
        // A current set has one bounded weak-subjectivity lifetime. Ordinary
        // same-epoch certificates may advance finality but cannot renew it.
        // A precommitted nextSet also cannot rotate after expiry: its proof has
        // no trusted signing timestamp, so accepting it later would reopen the
        // same long-range attack with keys from an obsolete set. Successful
        // initialization starts the clock; only a timely, verified epoch
        // handover resets it.
        if (block.timestamp - lastRotationTime > MAX_EPOCH_LAG) {
            revert EpochLagExceeded();
        }
        // An absolute epoch lower bound alone is insufficient after a relay
        // outage: once several lower bounds are in the past, a withheld
        // lineage could otherwise batch-walk those epochs in one transaction
        // sequence and repeatedly renew lastRotationTime. Every accepted
        // handover must consume at least one real minimum epoch as well.
        if (transition && block.timestamp - lastRotationTime < MIN_EPOCH_DURATION) revert EpochTimeWindow();
        if (!_epochTimeValid(finalizedBlock.epoch, block.timestamp)) {
            revert EpochTimeWindow();
        }

        if (finalizedBlock.height <= latest.height) revert NotMonotone();
        if (finalizedBlock.blockHash == bytes32(0) || finalizedBlock.validatorSetHash == bytes32(0)) {
            revert BadFinalizedBlock();
        }
        if (finalizedBlock.height < BRIDGE_ACTIVATION_HEIGHT && finalizedBlock.withdrawalRoot != bytes32(0)) {
            revert PrematureWithdrawalRoot();
        }
        if (finalizedBlock.height >= BRIDGE_ACTIVATION_HEIGHT && finalizedBlock.withdrawalRoot == bytes32(0)) {
            revert MissingWithdrawalRoot();
        }

        bytes32 successorHash = B3Types.hashSetHeader(successor);
        if (
            successor.epoch != finalizedBlock.epoch + 1 || !B3Types.headerShapeValid(successor)
                || successorHash != finalizedBlock.validatorSetHash
        ) revert BadSuccessor();

        B3Types.SetHeader memory signingSet;
        bytes32 signingSetHash;
        if (transition) {
            signingSet = nextSet;
            signingSetHash = nextSetHash;
        } else {
            signingSet = currentSet;
            signingSetHash = currentSetHash;
        }

        if (!prover.verify(CHAIN_DOMAIN, finalizedBlock, signingSetHash, signingSet, proof)) revert ProofRejected();

        bool signingSetBridgeReady = _bridgeThresholdMet(signingSet);
        if (signingSetBridgeReady && finalizedBlock.height >= BRIDGE_ACTIVATION_HEIGHT) {
            _latestBridgeFinalizedHeight = finalizedBlock.height;
            _latestBridgeWithdrawalRoot = finalizedBlock.withdrawalRoot;
        }

        // Rotate only after the successor-signed certificate has verified.
        if (transition) {
            currentSet = nextSet;
            currentSetHash = nextSetHash;
            currentEpoch += 1;
            delete nextSet;
            nextSetHash = bytes32(0);
            setHashByEpoch[currentEpoch] = currentSetHash;
        }

        if (nextSetHash == bytes32(0)) {
            nextSet = successor;
            nextSetHash = successorHash;
        } else if (nextSetHash != successorHash) {
            revert SuccessorMismatch();
        }

        latest = finalizedBlock;
        lastCertificateTime = block.timestamp;
        if (transition) {
            lastRotationTime = block.timestamp;
        }
        emit Finalized(
            finalizedBlock.epoch,
            finalizedBlock.height,
            finalizedBlock.blockHash,
            finalizedBlock.withdrawalRoot,
            _bitmapOf(proof)
        );
    }

    function latestWithdrawalRoot() external view returns (bytes32) {
        return latest.withdrawalRoot;
    }

    function latestFinalizedHeight() external view returns (uint64) {
        return latest.height;
    }

    /// Live finality readiness is intentionally stronger than chain finality.
    /// It requires the current B3 signing set AND its known successor to meet
    /// the public bridge thresholds, a threshold-qualified post-activation
    /// certificate, and a recent certificate with enough validator-set
    /// lifetime remaining. Requiring the successor identifies a scheduled
    /// handover to a bridge-ineligible set. The custody contract uses this same
    /// fail-closed predicate for deposits, so funds cannot enter before the
    /// contract has a live verifier/release path. B3 consensus may still keep
    /// creation of irreversible withdrawal leaves disabled behind its
    /// separate outbound height W; this contract cannot observe that B3-only
    /// release pin.
    function bridgeReady() public view returns (bool) {
        if (!initialized || lastCertificateTime == 0) return false;
        if (!_epochTimeValid(currentEpoch, block.timestamp)) return false;
        if (
            block.timestamp - lastCertificateTime > MAX_CERTIFICATE_AGE
                || block.timestamp - lastRotationTime >= MAX_EPOCH_LAG - MIN_DEPOSIT_EXIT_WINDOW
        ) return false;
        return _bridgeThresholdMet(currentSet) && nextSetHash != bytes32(0) && _bridgeThresholdMet(nextSet)
            && _latestBridgeFinalizedHeight >= BRIDGE_ACTIVATION_HEIGHT;
    }

    /// Custody is irreversible and has no owner/refund path. Accept deposits
    /// only while the complete bridge-readiness predicate holds; accepting them
    /// during bootstrap, a certificate outage, or a weak-set handover could
    /// otherwise lock funds permanently.
    function depositViable() public view returns (bool) {
        return bridgeReady();
    }

    /// A lag stop rejects new certificates, but it does not invalidate a
    /// withdrawal root that was already accepted from a fresh, bridge-qualified
    /// set. The vault may keep releasing exact proofs against that frozen root
    /// without granting any stale validator new authority.
    function releaseReady() public view returns (bool) {
        return initialized && _latestBridgeFinalizedHeight >= BRIDGE_ACTIVATION_HEIGHT;
    }

    function latestBridgeFinalizedHeight() external view returns (uint64) {
        return _latestBridgeFinalizedHeight;
    }

    function latestBridgeWithdrawalRoot() external view returns (bytes32) {
        return _latestBridgeWithdrawalRoot;
    }

    function epochTimeValid(uint64 epoch) external view returns (bool) {
        return initialized && _epochTimeValid(epoch, block.timestamp);
    }

    function hashSetHeader(B3Types.SetHeader calldata header) external pure returns (bytes32) {
        return B3Types.hashSetHeader(header);
    }

    function _bitmapOf(bytes calldata proof) private pure returns (bytes memory) {
        // V1 witness starts with abi.encode(bytes bitmap, ...). Event decoding
        // is observability only; malformed data never bypasses prover.verify.
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

    function _codeHash(address account) private view returns (bytes32 hash) {
        assembly {
            hash := extcodehash(account)
        }
    }

    function _bridgeThresholdMet(B3Types.SetHeader memory set) private view returns (bool) {
        return set.validatorCount >= MIN_BRIDGE_VALIDATORS && set.validatorCount <= MAX_BRIDGE_VALIDATORS
            && set.totalWeight >= MIN_BRIDGE_TOTAL_WEIGHT;
    }

    function _epochTimeValid(uint64 epoch, uint256 timestamp) private view returns (bool) {
        uint256 epochNumber = uint256(epoch);
        if (epochNumber > (type(uint256).max - GENESIS_TIME) / MIN_EPOCH_DURATION) return false;
        uint256 earliest = GENESIS_TIME + epochNumber * MIN_EPOCH_DURATION;

        uint256 epochAfter = epochNumber + 1;
        if (epochAfter > (type(uint256).max - GENESIS_TIME) / MAX_EPOCH_LAG) return false;
        uint256 latestAllowed = GENESIS_TIME + epochAfter * MAX_EPOCH_LAG;
        return timestamp >= earliest && timestamp <= latestAllowed;
    }
}
