// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

// REGTEST ONLY, never imported by src/ or script/.

/// Tether (USDT) test double for local anvil chains, written from scratch.
///
/// State is declared in the storage order of Ethereum-mainnet TetherToken
/// (0xdAC17F958D2ee523a2206206994597C13D831ec7, linearized as Ownable,
/// Pausable, ERC20Basic, BasicToken, StandardToken, BlackList, TetherToken),
/// so storage proofs against this mock use the slots B3 reads on mainnet:
///
///   slot 0   owner (bytes 0-19), paused (byte 20)
///   slot 1   _totalSupply
///   slot 2   balances
///   slot 3   basisPointsRate
///   slot 4   maximumFee
///   slot 5   allowed
///   slot 6   isBlackListed
///   slot 7   name
///   slot 8   symbol
///   slot 9   decimals (uint256 = 6)
///   slot 10  upgradedAddress (bytes 0-19), deprecated (byte 20)
///
/// test/regtest/MockTetherToken.t.sol pins every slot with vm.load.
///
/// Behaviour follows Tether where the bridge depends on it: approve, transfer
/// and transferFrom return no value; transfer and transferFrom revert without
/// data while paused or when the sender (transfer) or `from` (transferFrom)
/// is blacklisted; a non-zero allowance cannot be changed to another non-zero
/// value; an allowance of MAX_UINT is never decremented; issue credits the
/// owner. Deferred until RT4/RT9 need them, with the slots above unchanged:
/// destroyBlackFunds, forwarding after deprecate (deprecate only records the
/// flag and address here) and a non-zero fee (basisPointsRate and maximumFee
/// stay zero).
contract MockTetherToken {
    address public owner;
    bool public paused;
    uint256 public _totalSupply;
    mapping(address => uint256) public balances;
    uint256 public basisPointsRate;
    uint256 public maximumFee;
    mapping(address => mapping(address => uint256)) public allowed;
    mapping(address => bool) public isBlackListed;
    string public name;
    string public symbol;
    uint256 public decimals;
    address public upgradedAddress;
    bool public deprecated;

    uint256 public constant MAX_UINT = type(uint256).max;

    event Transfer(address indexed from, address indexed to, uint256 value);
    event Approval(address indexed owner, address indexed spender, uint256 value);
    event Issue(uint256 amount);
    event Pause();
    event Unpause();
    event AddedBlackList(address user);
    event RemovedBlackList(address user);
    event Deprecate(address newAddress);

    modifier onlyOwner() {
        require(msg.sender == owner);
        _;
    }

    modifier whenNotPaused() {
        require(!paused);
        _;
    }

    modifier whenPaused() {
        require(paused);
        _;
    }

    constructor() {
        owner = msg.sender;
        name = "Mock Tether USD";
        symbol = "USDT";
        decimals = 6;
    }

    function totalSupply() external view returns (uint256) {
        return _totalSupply;
    }

    function balanceOf(address who) external view returns (uint256) {
        return balances[who];
    }

    function allowance(address holder, address spender) external view returns (uint256) {
        return allowed[holder][spender];
    }

    function transfer(address to, uint256 value) external whenNotPaused {
        require(!isBlackListed[msg.sender]);
        _move(msg.sender, to, value);
    }

    function transferFrom(address from, address to, uint256 value) external whenNotPaused {
        require(!isBlackListed[from]);
        uint256 current = allowed[from][msg.sender];
        if (current < MAX_UINT) allowed[from][msg.sender] = current - value;
        _move(from, to, value);
    }

    function approve(address spender, uint256 value) external {
        require(value == 0 || allowed[msg.sender][spender] == 0);
        allowed[msg.sender][spender] = value;
        emit Approval(msg.sender, spender, value);
    }

    function issue(uint256 amount) external onlyOwner {
        require(amount != 0);
        balances[owner] += amount;
        _totalSupply += amount;
        emit Issue(amount);
    }

    function pause() external onlyOwner whenNotPaused {
        paused = true;
        emit Pause();
    }

    function unpause() external onlyOwner whenPaused {
        paused = false;
        emit Unpause();
    }

    function addBlackList(address user) external onlyOwner {
        isBlackListed[user] = true;
        emit AddedBlackList(user);
    }

    function removeBlackList(address user) external onlyOwner {
        isBlackListed[user] = false;
        emit RemovedBlackList(user);
    }

    function deprecate(address upgraded) external onlyOwner {
        deprecated = true;
        upgradedAddress = upgraded;
        emit Deprecate(upgraded);
    }

    function _move(address from, address to, uint256 value) private {
        balances[from] -= value;
        balances[to] += value;
        emit Transfer(from, to, value);
    }
}
