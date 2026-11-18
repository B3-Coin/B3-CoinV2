// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

/// Synthetic base assets for the owned Nitro laboratory, never USDT0 or a
/// production token. The native quote is a separately deployed real USDT0 code.
contract RebirthNitroLabToken {
    uint256 public constant LAB_CHAIN_ID = 412346;
    bool public constant LAB_ONLY = true;
    uint8 public constant decimals = 6;
    string public name;
    string public symbol;
    address public immutable minter;
    uint256 public totalSupply;
    mapping(address => uint256) public balanceOf;
    mapping(address => mapping(address => uint256)) public allowance;

    error NotNitroLab();
    error NotMinter();
    error InvalidAmount();
    error ZeroAddress();
    error InsufficientBalance();
    error InsufficientAllowance();
    error InvalidName();

    event Transfer(address indexed from, address indexed to, uint256 amount);
    event Approval(address indexed owner, address indexed spender, uint256 amount);

    modifier onlyLab() {
        if (block.chainid != LAB_CHAIN_ID) revert NotNitroLab();
        _;
    }

    constructor(string memory name_, string memory symbol_) onlyLab {
        if (bytes(name_).length == 0 || bytes(name_).length > 48 || bytes(symbol_).length == 0 || bytes(symbol_).length > 12)
            revert InvalidName();
        name = name_; symbol = symbol_; minter = msg.sender;
    }

    function mint(address to, uint256 amount) external onlyLab {
        if (msg.sender != minter) revert NotMinter();
        if (to == address(0)) revert ZeroAddress();
        if (amount == 0) revert InvalidAmount();
        totalSupply += amount; balanceOf[to] += amount;
        emit Transfer(address(0), to, amount);
    }

    function approve(address spender, uint256 amount) external onlyLab returns (bool) {
        allowance[msg.sender][spender] = amount;
        emit Approval(msg.sender, spender, amount);
        return true;
    }

    function transfer(address to, uint256 amount) external onlyLab returns (bool) {
        _move(msg.sender, to, amount);
        return true;
    }

    function transferFrom(address from, address to, uint256 amount) external onlyLab returns (bool) {
        uint256 permitted = allowance[from][msg.sender];
        if (permitted < amount) revert InsufficientAllowance();
        if (permitted != type(uint256).max) allowance[from][msg.sender] = permitted - amount;
        _move(from, to, amount);
        return true;
    }

    function _move(address from, address to, uint256 amount) private {
        if (to == address(0)) revert ZeroAddress();
        if (balanceOf[from] < amount) revert InsufficientBalance();
        balanceOf[from] -= amount; balanceOf[to] += amount;
        emit Transfer(from, to, amount);
    }
}
