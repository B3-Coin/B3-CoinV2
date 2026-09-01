// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

/// @title IB3FinalityProver — the swappable proof backend (spec §7 ZK seam).
///
/// The verifier state machine (§5.2) never changes; only the object that
/// answers "is this certificate valid for the committed set?" does. v1 ships
/// BlsCertificateProver (the §5.3 A–F pairing check); a later ZkFinalityProver
/// verifies a SNARK/STARK of the identical statement. The single
/// governance-changeable slot is the verifier's `prover` reference.
interface IB3FinalityProver {
    /// @param chainDomain 32-byte B3 modern chain domain (genesis || X).
    /// @param fb          the FinalizedBlock being attested (encoded per §3).
    /// @param setHash     keccak(SetHeader) of the SIGNING set (Set_epoch).
    /// @param set         the SetHeader of the signing set (its fields are
    ///                    trusted only insofar as keccak(set) == setHash,
    ///                    which the verifier checks before calling).
    /// @param proof       prover-specific witness (v1: bitmap+sig+absentees).
    /// @return ok         true iff the certificate satisfies §5.3 for `set`.
    function verify(
        bytes32 chainDomain,
        B3Types.FinalizedBlock calldata fb,
        bytes32 setHash,
        B3Types.SetHeader calldata set,
        bytes calldata proof
    ) external view returns (bool ok);
}

/// @title B3Types — the frozen wire structs shared by verifier, prover, bridge.
/// Layouts are consensus-relevant and MUST match the B3-side encoders in
/// src/flowmesh/bls_certificate.* and doc/design/b3-cross-chain-finality-v1.md.
library B3Types {
    /// §3: 112 bytes when encoded (8+32+32+32+8), big-endian fixed width.
    struct FinalizedBlock {
        uint64  height;            // finalized checkpoint height
        bytes32 blockHash;         // modern block hash at `height`
        bytes32 withdrawalRoot;    // §6 cumulative root; zero before bridge activation
        bytes32 validatorSetHash;  // keccak(header of Set_{epoch+1}) — successor, always
        uint64  epoch;             // epoch of the SIGNING set Set_epoch
    }

    /// §2: 110 bytes when encoded (8+2+4+8+8+48+32).
    struct SetHeader {
        uint64  epoch;
        uint16  rulesetVersion;    // 1
        uint32  validatorCount;    // n, 1..8192
        uint64  totalWeight;       // W
        uint64  quorumWeight;      // ruleset 1: floor(2W/3)+1
        bytes   aggregatePubkey;   // 48-byte compressed G1, Σ pk_i
        bytes32 membersRoot;       // root of the 8192-leaf keccak tree
    }
}
