// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

// REGTEST ONLY, never imported by src/ or script/.

/// Deposit-only stand-in for B3FinalityVerifier on local anvil chains
/// (regtest plan RT2 and RT3a).
///
/// It exposes exactly the views B3StakerBridge reads: CHAIN_DOMAIN and
/// BRIDGE_ACTIVATION_HEIGHT in the constructor, depositViable in deposit, and
/// releaseReady, latestBridgeFinalizedHeight and latestBridgeWithdrawalRoot in
/// release. Deposits are open until setDepositViable(false); the release path
/// is never ready, so B3StakerBridge.release reverts BridgeNotReady. It
/// verifies nothing, and anyone may toggle depositViable.
contract MockDepositVerifier {
    bytes32 public immutable CHAIN_DOMAIN;
    uint64 public immutable BRIDGE_ACTIVATION_HEIGHT;

    bool public depositViable = true;

    constructor(bytes32 chainDomain, uint64 bridgeActivationHeight) {
        CHAIN_DOMAIN = chainDomain;
        BRIDGE_ACTIVATION_HEIGHT = bridgeActivationHeight;
    }

    function setDepositViable(bool viable) external {
        depositViable = viable;
    }

    function releaseReady() external pure returns (bool) {
        return false;
    }

    function latestBridgeFinalizedHeight() external pure returns (uint64) {
        return 0;
    }

    function latestBridgeWithdrawalRoot() external pure returns (bytes32) {
        return bytes32(0);
    }
}
