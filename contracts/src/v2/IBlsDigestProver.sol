// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {B3Types} from "../IB3FinalityProver.sol";

/// Proof-backend seam of the bridge V2 verifier (contracts spec §3.6).
///
/// The implementation is V1's EIP-2537 BLS path and dual quorum
/// (contracts/src/BlsCertificateProver.sol) behind an entry point that takes a
/// precomputed message digest instead of hashing a V1 FinalizedBlock, so the
/// caller decides the statement format (digest V2, B3TypesV2.digest). The
/// signing set must be a ruleset-2 header (B3TypesV2.headerShapeValid) whose
/// keccak256 equals signingSetHash. The witness ABI is unchanged from V1:
/// abi.encode(bytes bitmap, bytes sig256, bytes aggPk128, Absent[] absent).
interface IBlsDigestProver {
    function verifyDigest(
        bytes32 digest,
        bytes32 signingSetHash,
        B3Types.SetHeader calldata signingSet,
        bytes calldata proof
    ) external view returns (bool ok);
}
