// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

/// Adversarial local fixture: changing the implementation leaves proxy runtime
/// bytecode unchanged. It is never deployed by the normal custody roundtrip.
contract RebirthCustodyProxyFixture {
    bytes32 private constant IMPLEMENTATION_SLOT =
        0x360894a13ba1a3210667c828492db98dca3e2076cc3735a920a3ca505d382bbc;
    address private immutable admin;
    error NotRegtest();
    error Forbidden();
    constructor(address implementation) {
        if (block.chainid != 31337) revert NotRegtest();
        admin = msg.sender;
        _set(implementation);
    }
    function upgradeTo(address implementation) external {
        if (block.chainid != 31337) revert NotRegtest();
        if (msg.sender != admin) revert Forbidden();
        _set(implementation);
    }
    function _set(address implementation) private {
        if (implementation.code.length == 0) revert Forbidden();
        assembly { sstore(IMPLEMENTATION_SLOT, implementation) }
    }
    fallback() external payable {
        if (block.chainid != 31337) revert NotRegtest();
        assembly {
            let target := sload(IMPLEMENTATION_SLOT)
            calldatacopy(0, 0, calldatasize())
            let success := delegatecall(gas(), target, 0, calldatasize(), 0, 0)
            returndatacopy(0, 0, returndatasize())
            switch success case 0 { revert(0, returndatasize()) } default { return(0, returndatasize()) }
        }
    }
}
