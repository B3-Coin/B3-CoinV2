// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

/// @title B3DepositVault — historical managed bridge prototype.
///
/// Deliberately minimal and trust-minimized: no owner, no pause, no upgrade.
/// Users lock ETH or ERC-20 tokens; the vault emits one canonical Deposit
/// event per lock with a strictly sequential id. B3 does NOT trust this
/// contract: the sync-committee light client in B3 proves the finalized
/// header, and a Merkle-Patricia receipt proof (src/bridge/mpt.h +
/// src/bridge/deposit.h) proves the event bytes. The event shape below is
/// consensus-relevant on the B3 side and MUST match bridge/deposit.h:
///
///   event Deposit(uint64 indexed depositId, address indexed token,
///                 uint256 amount, bytes32 b3Recipient);
///
/// Funds can leave only through `release`, callable exclusively by the
/// release authority. This source is retained to reproduce the published
/// managed smoke vault; the current transition release targets the keyless
/// B3StakerBridge/B3FinalityVerifier stack instead. A zero authority would trap
/// funds forever and the constructor refuses it.
contract B3DepositVault {
    /// Emitted once per lock. `token == address(0)` means native ETH.
    /// `amount` is the amount the vault ACTUALLY received (balance delta),
    /// so fee-on-transfer tokens cannot cause B3 to mint more than the
    /// vault holds. `b3Recipient` is opaque here; its semantics are defined
    /// by B3 consensus at the separately pinned bridge activation.
    event Deposit(uint64 indexed depositId, address indexed token, uint256 amount, bytes32 b3Recipient);

    event Released(address indexed token, address indexed to, uint256 amount);
    event Rescued(address indexed token, address indexed to, uint256 amount);

    uint64 public nextDepositId;
    address public immutable releaseAuthority;
    address public immutable rescueAuthority;

    /// Deposit-leg liabilities per token (address(0) = ETH): raised by every
    /// deposit, lowered by every release. `rescue` can only withdraw what
    /// sits ABOVE this figure, so even a compromised rescuer can never touch
    /// legitimately locked funds -- rescue is for strays only (tokens sent
    /// directly without depositToken, airdrops, force-sent ETH, deposits of
    /// tokens B3 never registers that the authority refunds off-band).
    mapping(address => uint256) public locked;

    error ZeroAuthority();
    error ZeroAmount();
    error NotAuthority();
    error TransferFailed();
    error Reentrancy();
    error NothingToRescue();

    uint256 private _entered;

    /// Blocks the balance-delta inflation a token with transfer hooks
    /// (ERC-777 style) could otherwise achieve by reentering depositToken
    /// mid-transfer: the inner deposit's delta would be double-counted by
    /// the outer call. USDT/standard ERC-20/ETH have no hooks, but the
    /// vault must be safe for ANY token a user throws at it.
    modifier nonReentrant() {
        if (_entered != 0) revert Reentrancy();
        _entered = 1;
        _;
        _entered = 0;
    }

    constructor(address _releaseAuthority, address _rescueAuthority) {
        if (_releaseAuthority == address(0) || _rescueAuthority == address(0)) revert ZeroAuthority();
        releaseAuthority = _releaseAuthority;
        rescueAuthority = _rescueAuthority;
    }

    /// Lock native ETH.
    function depositETH(bytes32 b3Recipient) external payable nonReentrant {
        if (msg.value == 0) revert ZeroAmount();
        locked[address(0)] += msg.value;
        emit Deposit(nextDepositId++, address(0), msg.value, b3Recipient);
    }

    /// Lock an ERC-20. Requires prior approve(). Records the balance delta,
    /// not the requested amount.
    function depositToken(address token, uint256 amount, bytes32 b3Recipient) external nonReentrant {
        if (amount == 0) revert ZeroAmount();
        uint256 before = _balanceOf(token, address(this));
        _safeTransferFrom(token, msg.sender, address(this), amount);
        uint256 received = _balanceOf(token, address(this)) - before;
        if (received == 0) revert ZeroAmount();
        locked[token] += received;
        emit Deposit(nextDepositId++, token, received, b3Recipient);
    }

    /// Historical managed withdrawal path — only the immutable release
    /// authority. This is not the current decentralized production path.
    function release(address token, address payable to, uint256 amount) external nonReentrant {
        if (msg.sender != releaseAuthority) revert NotAuthority();
        uint256 l = locked[token];
        locked[token] = amount >= l ? 0 : l - amount; // saturating: accounting bounds rescue, never the authority
        if (token == address(0)) {
            (bool ok,) = to.call{value: amount}("");
            if (!ok) revert TransferFailed();
        } else {
            _safeTransfer(token, to, amount);
        }
        emit Released(token, to, amount);
    }

    /// Withdraw ONLY the surplus above the deposit-leg liabilities: strays,
    /// airdrops, force-sent ETH. Structurally unable to touch locked funds.
    function rescue(address token, address payable to) external nonReentrant {
        if (msg.sender != rescueAuthority) revert NotAuthority();
        uint256 balance = token == address(0) ? address(this).balance : _balanceOf(token, address(this));
        uint256 l = locked[token];
        if (balance <= l) revert NothingToRescue();
        uint256 surplus = balance - l;
        if (token == address(0)) {
            (bool ok,) = to.call{value: surplus}("");
            if (!ok) revert TransferFailed();
        } else {
            _safeTransfer(token, to, surplus);
        }
        emit Rescued(token, to, surplus);
    }

    // --- minimal safe-ERC20 (no library dependencies) ---------------------

    function _balanceOf(address token, address who) private view returns (uint256) {
        (bool ok, bytes memory data) = token.staticcall(abi.encodeWithSignature("balanceOf(address)", who));
        if (!ok || data.length < 32) revert TransferFailed();
        return abi.decode(data, (uint256));
    }

    function _safeTransferFrom(address token, address from, address to, uint256 amount) private {
        (bool ok, bytes memory data) =
            token.call(abi.encodeWithSignature("transferFrom(address,address,uint256)", from, to, amount));
        if (!ok || (data.length != 0 && !abi.decode(data, (bool)))) revert TransferFailed();
    }

    function _safeTransfer(address token, address to, uint256 amount) private {
        (bool ok, bytes memory data) = token.call(abi.encodeWithSignature("transfer(address,uint256)", to, amount));
        if (!ok || (data.length != 0 && !abi.decode(data, (bool)))) revert TransferFailed();
    }
}
