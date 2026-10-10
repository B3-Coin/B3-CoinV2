// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

/// Per-user forwarder account of bridge V2 (doc/design/b3-bridge-v2-contracts-spec.md §7.2).
///
/// Both forwarder factories (B3DepositForwarderFactory, B3OftExitFactory)
/// CREATE2-deploy this exact init code. It takes no constructor arguments:
/// FACTORY is the deployer, so the init code, and therefore the manifest's
/// FORWARDER_INIT_CODE_HASH, is the same for both factories and an account
/// address can be derived, and paid, before the account exists.
///
/// The account holds tokens and does only what its factory tells it through
/// exec; all policy (who may move what, fee caps, expiry) lives in the
/// factory. It has no receive or fallback function, so after deployment it
/// accepts ETH only through exec. This file imports nothing, so its metadata,
/// and the init-code hash B3 pins, depends on no other source.
contract B3Forwarder {
    /// The factory that deployed this account; the only caller of exec.
    address public immutable FACTORY;

    error NotFactory();

    constructor() {
        FACTORY = msg.sender;
    }

    /// Calls `target` with `value` wei of this account's balance and `data`,
    /// returning the callee's return data and bubbling its revert data.
    function exec(address target, uint256 value, bytes calldata data) external payable returns (bytes memory result) {
        if (msg.sender != FACTORY) revert NotFactory();
        bool ok;
        (ok, result) = target.call{value: value}(data);
        if (!ok) {
            assembly ("memory-safe") {
                revert(add(result, 0x20), mload(result))
            }
        }
    }
}
