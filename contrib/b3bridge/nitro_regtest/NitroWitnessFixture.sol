// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

/// Owned Nitro dev-chain witness producer. This is not a stable token or vault.
contract NitroWitnessFixture {
    uint256 public value;
    address public lastWriter;
    uint256 public nonce;
    event Witness(address indexed caller, uint256 value);
    event Deposit(uint64 indexed depositId, bytes32 indexed account,
                  bytes32 indexed asset, uint256 amount, address sender);
    event Withdrawn(bytes32 indexed id, address indexed recipient, uint256 amount);

    constructor() {
        require(block.chainid == 412346, "owned Nitro lab only");
    }

    function write(uint256 next) external {
        require(block.chainid == 412346, "owned Nitro lab only");
        value = next;
        lastWriter = msg.sender;
        emit Witness(msg.sender, next);
    }

    // Event decoder fixtures only: these functions do not custody an ERC20.
    function emitDeposit(bytes32 account, bytes32 asset, uint256 amount) external {
        require(block.chainid == 412346, "owned Nitro lab only");
        require(amount > 0 && nonce < type(uint64).max, "fixture bound");
        value += amount;
        lastWriter = msg.sender;
        ++nonce;
        emit Deposit(uint64(nonce), account, asset, amount, msg.sender);
    }

    function emitPayout(bytes32 id, address recipient, uint256 amount) external {
        require(block.chainid == 412346, "owned Nitro lab only");
        require(amount > 0 && amount <= value, "fixture bound");
        value -= amount;
        lastWriter = recipient;
        ++nonce;
        emit Withdrawn(id, recipient, amount);
    }
}
