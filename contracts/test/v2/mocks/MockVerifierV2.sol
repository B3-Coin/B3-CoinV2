// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {B3Types} from "../../../src/IB3FinalityProver.sol";
import {B3TypesV2} from "../../../src/v2/B3TypesV2.sol";
import {IB3FinalityVerifierV2} from "../../../src/v2/IB3FinalityVerifierV2.sol";

/// TEST ONLY. Stand-in for B3FinalityVerifierV2 exposing exactly what
/// B3VaultV2 reads: CHAIN_DOMAIN and BRIDGE_ACTIVATION_HEIGHT at construction,
/// depositsOpen in deposit and acceptedRootHeight in release. Every value is
/// set directly by the test; it verifies nothing. Like the real verifier it
/// writes each root's height once unless `overwrite` is used.
contract MockVerifierV2 is IB3FinalityVerifierV2 {
    bytes32 public immutable CHAIN_DOMAIN;
    uint64 public immutable BRIDGE_ACTIVATION_HEIGHT;

    bool public depositsOpen;
    uint8 public status = B3TypesV2.STATUS_LIVE;
    mapping(bytes32 root => uint64 height) public acceptedRootHeight;

    error MockOnly();
    error RootAlreadyAccepted();

    constructor(bytes32 chainDomain, uint64 bridgeActivationHeight) {
        CHAIN_DOMAIN = chainDomain;
        BRIDGE_ACTIVATION_HEIGHT = bridgeActivationHeight;
    }

    function setDepositsOpen(bool open) external {
        depositsOpen = open;
    }

    function setStatus(uint8 newStatus) external {
        status = newStatus;
    }

    /// Accepts `root` at `height`, once (the real verifier's I-5).
    function acceptRoot(bytes32 root, uint64 height) external {
        if (acceptedRootHeight[root] != 0) revert RootAlreadyAccepted();
        acceptedRootHeight[root] = height;
    }

    /// Test escape hatch for heights the real verifier could never record.
    function overwriteRootHeight(bytes32 root, uint64 height) external {
        acceptedRootHeight[root] = height;
    }

    function submitCertificate(
        B3TypesV2.FinalizedBlockV2 calldata,
        B3Types.SetHeader calldata,
        B3Types.SetHeader calldata,
        bytes32[32] calldata,
        bytes calldata
    ) external pure {
        revert MockOnly();
    }
}
