// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {B3Types, IB3FinalityProver} from "./IB3FinalityProver.sol";

/// Permissionless Ethereum mirror of B3's stake-weighted finality lineage.
///
/// This contract deliberately has no owner, pause key, upgrade proxy, or
/// emergency signer. Authority starts with the public Set_0 snapshot produced
/// by B3 at M-1 and advances only when the current stake/BLS quorum signs its
/// successor. Deployment is therefore possible only after that snapshot is
/// known; it is not tied to an Ethereum transaction at the exact B3 M block.
///
/// Bootstrap is fail-closed: the deployer supplies the expected public Set_0
/// hash, proof-backend runtime hash, minimum bridge validator count, and minimum
/// bridge total staked weight. A small Set_0 may still be tracked so the chain
/// can bootstrap; it cannot authorize deposits or withdrawals until the
/// current signing set satisfies both bridge thresholds.
contract B3FinalityVerifier {
    bytes32 public immutable CHAIN_DOMAIN;
    bytes32 public immutable GENESIS_SET_HASH;
    bytes32 public immutable PROVER_CODE_HASH;
    uint64 public immutable MODERN_START_HEIGHT;
    uint64 public immutable BRIDGE_ACTIVATION_HEIGHT;
    uint32 public immutable MIN_BRIDGE_VALIDATORS;
    uint64 public immutable MIN_BRIDGE_TOTAL_WEIGHT;
    uint256 public immutable MAX_EPOCH_LAG;
    IB3FinalityProver public immutable prover;

    uint64 public currentEpoch;
    bytes32 public currentSetHash;
    B3Types.SetHeader public currentSet;
    bytes32 public nextSetHash;
    B3Types.SetHeader public nextSet;
    mapping(uint64 => bytes32) public setHashByEpoch;
    B3Types.FinalizedBlock public latest;
    uint256 public lastCertificateTime;
    uint64 private _latestBridgeFinalizedHeight;
    bytes32 private _latestBridgeWithdrawalRoot;

    event Finalized(
        uint64 indexed epoch,
        uint64 indexed height,
        bytes32 blockHash,
        bytes32 withdrawalRoot,
        bytes signerBitmap
    );

    error BadBootstrap();
    error BadGenesisSet();
    error BadProver();
    error EpochNotAccepted();
    error NotMonotone();
    error BadFinalizedBlock();
    error PrematureWithdrawalRoot();
    error BadSuccessor();
    error SuccessorMismatch();
    error EpochLagExceeded();
    error ProofRejected();

    constructor(
        bytes32 chainDomain,
        B3Types.SetHeader memory genesisSet,
        bytes32 expectedGenesisSetHash,
        IB3FinalityProver prover_,
        bytes32 expectedProverCodeHash,
        uint64 modernStartHeight,
        uint64 bridgeActivationHeight,
        uint32 minBridgeValidators,
        uint64 minBridgeTotalWeight,
        uint256 maxEpochLag
    ) {
        if (
            chainDomain == bytes32(0) ||
            expectedGenesisSetHash == bytes32(0) ||
            expectedProverCodeHash == bytes32(0) ||
            modernStartHeight == 0 ||
            bridgeActivationHeight < modernStartHeight ||
            minBridgeValidators < 4 ||
            minBridgeValidators > 8_192 ||
            minBridgeTotalWeight == 0 ||
            maxEpochLag == 0
        ) revert BadBootstrap();

        if (
            genesisSet.epoch != 0 ||
            !B3Types.headerShapeValid(genesisSet) ||
            B3Types.hashSetHeader(genesisSet) != expectedGenesisSetHash
        ) revert BadGenesisSet();

        if (
            address(prover_) == address(0) ||
            address(prover_).code.length == 0 ||
            _codeHash(address(prover_)) != expectedProverCodeHash
        ) revert BadProver();

        CHAIN_DOMAIN = chainDomain;
        GENESIS_SET_HASH = expectedGenesisSetHash;
        PROVER_CODE_HASH = expectedProverCodeHash;
        MODERN_START_HEIGHT = modernStartHeight;
        BRIDGE_ACTIVATION_HEIGHT = bridgeActivationHeight;
        MIN_BRIDGE_VALIDATORS = minBridgeValidators;
        MIN_BRIDGE_TOTAL_WEIGHT = minBridgeTotalWeight;
        MAX_EPOCH_LAG = maxEpochLag;
        prover = prover_;

        currentSet = genesisSet;
        currentSetHash = expectedGenesisSetHash;
        setHashByEpoch[0] = expectedGenesisSetHash;
        latest = B3Types.FinalizedBlock({
            height: modernStartHeight - 1,
            blockHash: bytes32(0),
            withdrawalRoot: bytes32(0),
            validatorSetHash: bytes32(0),
            epoch: 0
        });
        // Deliberately zero until the first accepted certificate. Deploying a
        // verifier before relay starts must not make its first real update
        // stale merely because Ethereum wall-clock time passed meanwhile.
        lastCertificateTime = 0;
    }

    /// Anyone may relay a certificate. No relayer gains authority: acceptance
    /// depends only on the pinned B3 lineage and its stake/BLS quorum.
    function submitCertificate(
        B3Types.FinalizedBlock calldata finalizedBlock,
        B3Types.SetHeader calldata successor,
        bytes calldata proof
    ) external {
        bool transition =
            finalizedBlock.epoch == currentEpoch + 1 &&
            nextSetHash != bytes32(0);
        if (finalizedBlock.epoch != currentEpoch && !transition) {
            revert EpochNotAccepted();
        }
        // Apply the weak-subjectivity/liveness stop to EVERY certificate, not
        // only rotations. Otherwise an indefinitely stale current epoch could
        // continue authorizing roots. The first certificate starts the clock.
        if (
            lastCertificateTime != 0 &&
            block.timestamp - lastCertificateTime > MAX_EPOCH_LAG
        ) revert EpochLagExceeded();

        if (finalizedBlock.height <= latest.height) revert NotMonotone();
        if (
            finalizedBlock.blockHash == bytes32(0) ||
            finalizedBlock.validatorSetHash == bytes32(0)
        ) revert BadFinalizedBlock();
        if (
            finalizedBlock.height < BRIDGE_ACTIVATION_HEIGHT &&
            finalizedBlock.withdrawalRoot != bytes32(0)
        ) revert PrematureWithdrawalRoot();

        bytes32 successorHash = B3Types.hashSetHeader(successor);
        if (
            successor.epoch != finalizedBlock.epoch + 1 ||
            !B3Types.headerShapeValid(successor) ||
            successorHash != finalizedBlock.validatorSetHash
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

        if (
            !prover.verify(
                CHAIN_DOMAIN,
                finalizedBlock,
                signingSetHash,
                signingSet,
                proof
            )
        ) revert ProofRejected();

        bool signingSetBridgeReady = _bridgeThresholdMet(signingSet);
        if (
            signingSetBridgeReady &&
            finalizedBlock.height >= BRIDGE_ACTIVATION_HEIGHT
        ) {
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

    /// Readiness is intentionally stronger than chain finality. It requires
    /// the CURRENT B3 signing set to meet the public bridge thresholds and at
    /// least one threshold-qualified certificate at/after bridge activation.
    function bridgeReady() public view returns (bool) {
        return
            lastCertificateTime != 0 &&
            block.timestamp - lastCertificateTime <= MAX_EPOCH_LAG &&
            _bridgeThresholdMet(currentSet) &&
            _latestBridgeFinalizedHeight >= BRIDGE_ACTIVATION_HEIGHT;
    }

    function latestBridgeFinalizedHeight() external view returns (uint64) {
        return _latestBridgeFinalizedHeight;
    }

    function latestBridgeWithdrawalRoot() external view returns (bytes32) {
        return _latestBridgeWithdrawalRoot;
    }

    function hashSetHeader(B3Types.SetHeader calldata header)
        external
        pure
        returns (bytes32)
    {
        return B3Types.hashSetHeader(header);
    }

    function _bitmapOf(bytes calldata proof)
        private
        pure
        returns (bytes memory)
    {
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

    function _bridgeThresholdMet(B3Types.SetHeader memory set)
        private
        view
        returns (bool)
    {
        return
            set.validatorCount >= MIN_BRIDGE_VALIDATORS &&
            set.totalWeight >= MIN_BRIDGE_TOTAL_WEIGHT;
    }
}
