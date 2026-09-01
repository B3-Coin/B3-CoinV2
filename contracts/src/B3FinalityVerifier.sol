// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {B3Types, IB3FinalityProver} from "./IB3FinalityProver.sol";

/// @title B3FinalityVerifier — the Ethereum attestation of B3 modern finality.
///
/// Implements doc/design/b3-cross-chain-finality-v1.md §5 EXACTLY: it accepts a
/// finality certificate only from the current signing set (or the disclosed
/// successor, which performs the epoch rotation before the signature is
/// checked), enforces monotone finalized height, binds the successor set hash,
/// and delegates the cryptographic check to a swappable prover (§5.3 / §7).
///
/// The contract stores NO validator list — only committed set HEADERS and the
/// single latest FinalizedBlock. Signer identities travel in events for
/// accountability, never in storage. There is NO owner and NO upgrade path; the
/// `prover` reference is immutable in v1 (a governance-swap variant would make
/// exactly that one slot settable behind its own timelock, out of v1 scope).
contract B3FinalityVerifier {
    // --- immutable genesis commitments (spec §5.1) ------------------------
    bytes32 public immutable CHAIN_DOMAIN;      // genesis || X (32 B)
    bytes32 public immutable GENESIS_SET_HASH;  // keccak(header of Set_0)
    uint64  public immutable GENESIS_EPOCH;     // = 0
    IB3FinalityProver public immutable prover;

    // --- rotation-tracked state ------------------------------------------
    uint64  public currentEpoch;
    bytes32 public currentSetHash;
    B3Types.SetHeader public currentSet;

    bytes32 public nextSetHash;                 // zero until the successor is learned
    B3Types.SetHeader public nextSet;

    mapping(uint64 => bytes32) public setHashByEpoch;   // append-only history
    B3Types.FinalizedBlock public latest;               // height strictly increasing
    uint256 public lastRotationTime;

    /// Ethereum-side liveness bound: an epoch that has been disclosed but not
    /// rotated within MAX_EPOCH_LAG marks the lineage stale on rotation
    /// (spec §9; the only bridge-activation-tunable numeric).
    uint256 public immutable MAX_EPOCH_LAG;

    event Finalized(uint64 indexed epoch, uint64 indexed height,
                    bytes32 blockHash, bytes32 withdrawalRoot, bytes signerBitmap);

    error BadGenesisHeader();
    error EpochNotAccepted();
    error NotMonotone();
    error BadSuccessor();
    error ProofRejected();
    error SuccessorMismatch();
    error EpochLagExceeded();

    constructor(
        bytes32 chainDomain,
        B3Types.SetHeader memory genesisSet,
        IB3FinalityProver prover_,
        uint256 maxEpochLag,
        uint64 mMinusOne
    ) {
        if (!_checkHeaderRules(genesisSet) || genesisSet.epoch != 0) revert BadGenesisHeader();
        CHAIN_DOMAIN = chainDomain;
        GENESIS_EPOCH = 0;
        prover = prover_;
        MAX_EPOCH_LAG = maxEpochLag;

        bytes32 h = _hashHeader(genesisSet);
        GENESIS_SET_HASH = h;
        currentEpoch = 0;
        currentSet = genesisSet;
        currentSetHash = h;
        setHashByEpoch[0] = h;
        // latest = {M-1, 0, 0, 0, 0}
        latest = B3Types.FinalizedBlock({
            height: mMinusOne, blockHash: 0, withdrawalRoot: 0,
            validatorSetHash: 0, epoch: 0
        });
        lastRotationTime = block.timestamp;
    }

    /// Spec §5.2, steps 1..7 in order.
    function submitCertificate(
        B3Types.FinalizedBlock calldata fb,
        B3Types.SetHeader calldata successor,
        bytes calldata proof
    ) external {
        // 1: only the current signing set, or the disclosed successor.
        bool transition = (fb.epoch == currentEpoch + 1) && (nextSetHash != bytes32(0));
        if (fb.epoch != currentEpoch && !transition) revert EpochNotAccepted();

        // 2: EPOCH TRANSITION — rotate BEFORE verifying, so the signature is
        //    checked against the set that actually signed it.
        if (transition) {
            currentSet = nextSet;
            currentSetHash = nextSetHash;
            currentEpoch += 1;
            delete nextSet;
            nextSetHash = bytes32(0);
            setHashByEpoch[currentEpoch] = currentSetHash;
            if (block.timestamp - lastRotationTime > MAX_EPOCH_LAG) revert EpochLagExceeded();
            lastRotationTime = block.timestamp;
        }

        // 3: monotone finalized height.
        if (fb.height <= latest.height) revert NotMonotone();

        // 4: successor header well-formed and bound to fb.validatorSetHash.
        if (successor.epoch != fb.epoch + 1
            || !_checkHeaderRules(successor)
            || _hashHeader(successor) != fb.validatorSetHash) revert BadSuccessor();

        // 5: cryptographic verification against the CURRENT (signing) set.
        if (!prover.verify(CHAIN_DOMAIN, fb, currentSetHash, currentSet, proof)) revert ProofRejected();

        // 6: learn / confirm the one successor for this epoch.
        if (nextSetHash == bytes32(0)) {
            nextSet = successor;
            nextSetHash = fb.validatorSetHash;
        } else if (nextSetHash != fb.validatorSetHash) {
            revert SuccessorMismatch();
        }

        // 7: commit and emit (bitmap surfaced for accountability).
        latest = fb;
        emit Finalized(fb.epoch, fb.height, fb.blockHash, fb.withdrawalRoot, _bitmapOf(proof));
    }

    // --- reads (spec §5.5) ------------------------------------------------
    function latestWithdrawalRoot() external view returns (bytes32) { return latest.withdrawalRoot; }

    // --- §5.4 header rules ------------------------------------------------
    function _checkHeaderRules(B3Types.SetHeader memory h) internal pure returns (bool) {
        if (h.rulesetVersion != 1) return false;
        if (h.validatorCount < 1 || h.validatorCount > 8192) return false;
        if (h.totalWeight == 0) return false;
        if (h.quorumWeight != (2 * h.totalWeight) / 3 + 1) return false;
        if (h.aggregatePubkey.length != 48) return false;
        // aggregatePubkey != INFINITY: the compressed-infinity encoding has the
        // second-most-significant bit set with all other bits zero.
        if (_isG1Infinity(h.aggregatePubkey)) return false;
        return true;
    }

    /// keccak(ValidatorSetHeader) over the exact 110-byte big-endian encoding.
    function _hashHeader(B3Types.SetHeader memory h) internal pure returns (bytes32) {
        return keccak256(abi.encodePacked(
            h.epoch, h.rulesetVersion, h.validatorCount, h.totalWeight,
            h.quorumWeight, h.aggregatePubkey, h.membersRoot));
    }

    function _isG1Infinity(bytes memory pk48) internal pure returns (bool) {
        if (pk48.length != 48) return false;
        // compressed infinity: 0xc0 0x00 ... 0x00
        if (uint8(pk48[0]) != 0xc0) return false;
        for (uint256 i = 1; i < 48; i++) if (pk48[i] != 0) return false;
        return true;
    }

    /// The signer bitmap is the head of the v1 proof witness (see prover ABI);
    /// decoded here only to surface it in the Finalized event. A prover whose
    /// witness has no bitmap (e.g. a future ZK prover) returns empty bytes.
    function _bitmapOf(bytes calldata proof) internal pure returns (bytes memory) {
        if (proof.length < 64) return "";
        // proof = abi.encode(bytes bitmap, bytes sig, Absent[] absent, bytes32[] multiproof, bool[] flags)
        // the first head word is the offset to `bitmap`.
        uint256 off;
        assembly { off := calldataload(proof.offset) }
        if (off + 32 > proof.length) return "";
        uint256 len;
        assembly { len := calldataload(add(proof.offset, off)) }
        if (off + 32 + len > proof.length) return "";
        return proof[off + 32 : off + 32 + len];
    }
}
