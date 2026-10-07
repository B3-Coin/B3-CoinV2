// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {B3Types} from "../IB3FinalityProver.sol";
import {B3TypesV2} from "./B3TypesV2.sol";

/// What B3VaultV2 and B3RelayBatch need from B3FinalityVerifierV2
/// (contracts spec §3). Status values are B3TypesV2.STATUS_*; callers treat
/// only STATUS_LIVE (1) as live.
interface IB3FinalityVerifierV2 {
    function CHAIN_DOMAIN() external view returns (bytes32);

    /// V: withdrawal roots are zero below it and non-zero from it.
    function BRIDGE_ACTIVATION_HEIGHT() external view returns (uint64);

    function status() external view returns (uint8);

    function depositsOpen() external view returns (bool);

    /// First B3 height at which `root` was accepted from a bridge-qualified set;
    /// zero if never accepted. Written once per root.
    function acceptedRootHeight(bytes32 root) external view returns (uint64);

    function submitCertificate(
        B3TypesV2.FinalizedBlockV2 calldata fb,
        B3Types.SetHeader calldata signingSet,
        B3Types.SetHeader calldata successor,
        bytes32[32] calldata ancestryPath,
        bytes calldata proof
    ) external;
}
