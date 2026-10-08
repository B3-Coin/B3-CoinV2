// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

// REGTEST ONLY, never imported by src/ or script/.

import {MockOFTBase} from "./MockOFTBase.sol";

/// USDT0 test double on the Arbitrum anvil (31338, endpoint id 30110),
/// written from scratch: a 6-decimal ERC20 that is its own OFT, so `token()`
/// is this contract and senders need no approval. It stands in for the
/// Arbitrum USDT0 token and OFT pair (the OFT 0x14E4A1B13bf7F943c8ff7C51fb60FA964A298D92
/// of contracts spec §7.4) in one contract.
///
/// Route (b): a user's `send` to dstEid 30101 burns from msg.sender and
/// messages MockOFTAdapter, which pays F_d in USDT. Route (d3): a delivery
/// from the adapter mints to `to`.
///
/// Blocklist after Tether's USDT0 token (contracts spec §8): transfer and
/// transferFrom revert when `from` is blocked, and a blocked holder cannot
/// send; only `from` is checked, so a delivery to a blocked `to` still mints
/// and the tokens arrive frozen. The owner may destroyBlockedFunds. Like
/// Tether, transfers to the token contract itself are refused; as in
/// LayerZero's OFT, a delivery to address(0) mints to 0x…dEaD.
///
/// Owner: mint (seeding), addBlocked, removeBlocked, destroyBlockedFunds,
/// and the peers and bridge fee of MockOFTBase. An allowance of
/// type(uint256).max is never decremented.
contract MockUSDT0 is MockOFTBase {
    string public constant name = "Mock USDT0";
    string public constant symbol = "USDT0";
    uint8 public constant decimals = 6;

    uint256 public totalSupply;
    mapping(address account => uint256) public balanceOf;
    mapping(address holder => mapping(address spender => uint256)) public allowance;
    mapping(address account => bool) public isBlocked;

    event Transfer(address indexed from, address indexed to, uint256 value);
    event Approval(address indexed owner, address indexed spender, uint256 value);
    event Mint(address indexed destination, uint256 amount);
    event BlockPlaced(address indexed user);
    event BlockReleased(address indexed user);
    event DestroyedBlockedFunds(address indexed blockedUser, uint256 balance);

    error Blocked(address account);
    error NotBlocked(address account);
    error BadReceiver(address to);
    error InsufficientBalance(address account, uint256 balance, uint256 needed);
    error InsufficientAllowance(address spender, uint256 allowance, uint256 needed);

    constructor(address endpoint_) MockOFTBase(endpoint_) {}

    function token() public view override returns (address) {
        return address(this);
    }

    function approvalRequired() external pure override returns (bool) {
        return false;
    }

    // ================================================================== ERC20

    function transfer(address to, uint256 value) external returns (bool) {
        _transfer(msg.sender, to, value);
        return true;
    }

    function transferFrom(address from, address to, uint256 value) external returns (bool) {
        uint256 current = allowance[from][msg.sender];
        if (current != type(uint256).max) {
            if (current < value) revert InsufficientAllowance(msg.sender, current, value);
            allowance[from][msg.sender] = current - value;
        }
        _transfer(from, to, value);
        return true;
    }

    function approve(address spender, uint256 value) external returns (bool) {
        allowance[msg.sender][spender] = value;
        emit Approval(msg.sender, spender, value);
        return true;
    }

    // ================================================================== owner

    function mint(address to, uint256 amount) external onlyOwner {
        if (to == address(0)) revert BadReceiver(to);
        _mint(to, amount);
        emit Mint(to, amount);
    }

    function addBlocked(address user) external onlyOwner {
        isBlocked[user] = true;
        emit BlockPlaced(user);
    }

    function removeBlocked(address user) external onlyOwner {
        isBlocked[user] = false;
        emit BlockReleased(user);
    }

    /// Burns the whole balance of a blocked holder.
    function destroyBlockedFunds(address user) external onlyOwner {
        if (!isBlocked[user]) revert NotBlocked(user);
        uint256 funds = balanceOf[user];
        _burn(user, funds);
        emit DestroyedBlockedFunds(user, funds);
    }

    // ===================================================================== OFT

    function _debit(address from, uint256 amount) internal override {
        if (isBlocked[from]) revert Blocked(from);
        _burn(from, amount);
    }

    function _credit(address to, uint256 amount) internal override returns (address) {
        if (to == address(0)) to = address(0xdEaD);
        _mint(to, amount);
        return to;
    }

    // =============================================================== internal

    function _transfer(address from, address to, uint256 value) private {
        if (isBlocked[from]) revert Blocked(from);
        if (to == address(0) || to == address(this)) revert BadReceiver(to);
        uint256 balance = balanceOf[from];
        if (balance < value) revert InsufficientBalance(from, balance, value);
        unchecked {
            balanceOf[from] = balance - value;
        }
        balanceOf[to] += value;
        emit Transfer(from, to, value);
    }

    function _mint(address to, uint256 amount) private {
        totalSupply += amount;
        balanceOf[to] += amount;
        emit Transfer(address(0), to, amount);
    }

    function _burn(address from, uint256 amount) private {
        uint256 balance = balanceOf[from];
        if (balance < amount) revert InsufficientBalance(from, balance, amount);
        unchecked {
            balanceOf[from] = balance - amount;
            totalSupply -= amount;
        }
        emit Transfer(from, address(0), amount);
    }
}
