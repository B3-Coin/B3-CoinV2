// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

/// Synthetic six-decimal Q for an unforked local EVM. Never a production asset.
/// The fixture minter can stop transfers to exercise unpaid-claim recovery.
contract RebirthRegtestToken {
    string public constant name = "Rebirth regtest Q";
    string public constant symbol = "rUSD";
    uint8 public constant decimals = 6;
    uint256 public constant REGTEST_CHAIN_ID = 31337;
    address public immutable minter;
    uint256 public totalSupply;
    bool public transfersEnabled = true;
    mapping(address => uint256) public balanceOf;
    mapping(address => mapping(address => uint256)) public allowance;

    error NotRegtest();
    error NotMinter();
    error InvalidAmount();
    error ZeroAddress();
    error TransfersDisabled();
    error InsufficientBalance();
    error InsufficientAllowance();

    event Transfer(address indexed from, address indexed to, uint256 amount);
    event Approval(address indexed owner, address indexed spender, uint256 amount);

    modifier onlyRegtest() {
        if (block.chainid != REGTEST_CHAIN_ID) revert NotRegtest();
        _;
    }

    constructor() onlyRegtest {
        minter = msg.sender;
    }

    function mint(address to, uint256 amount) external onlyRegtest {
        if (msg.sender != minter) revert NotMinter();
        if (to == address(0)) revert ZeroAddress();
        if (amount == 0) revert InvalidAmount();
        totalSupply += amount;
        balanceOf[to] += amount;
        emit Transfer(address(0), to, amount);
    }

    function setTransfersEnabled(bool enabled) external onlyRegtest {
        if (msg.sender != minter) revert NotMinter();
        transfersEnabled = enabled;
    }

    function approve(address spender, uint256 amount) external onlyRegtest returns (bool) {
        allowance[msg.sender][spender] = amount;
        emit Approval(msg.sender, spender, amount);
        return true;
    }

    function transfer(address to, uint256 amount) external onlyRegtest returns (bool) {
        _move(msg.sender, to, amount);
        return true;
    }

    function transferFrom(address from, address to, uint256 amount) external onlyRegtest returns (bool) {
        uint256 allowed = allowance[from][msg.sender];
        if (allowed < amount) revert InsufficientAllowance();
        if (allowed != type(uint256).max) allowance[from][msg.sender] = allowed - amount;
        _move(from, to, amount);
        return true;
    }

    function _move(address from, address to, uint256 amount) private {
        if (!transfersEnabled) revert TransfersDisabled();
        if (to == address(0)) revert ZeroAddress();
        if (balanceOf[from] < amount) revert InsufficientBalance();
        balanceOf[from] -= amount;
        balanceOf[to] += amount;
        emit Transfer(from, to, amount);
    }
}
