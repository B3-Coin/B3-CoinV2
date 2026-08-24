// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

/// @title B3DepositVault — the Ethereum side of the ETH -> B3 deposit leg.
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
/// release authority — the B3 finality verifier / bridge contract of
/// doc/design/b3-cross-chain-finality-v1.md §5-§6 (B3 -> ETH withdrawals).
/// Deploy the vault together with (or after) that verifier; a vault with a
/// zero release authority would trap funds forever and the constructor
/// refuses it.
contract B3DepositVault {
    /// Emitted once per lock. `token == address(0)` means native ETH.
    /// `amount` is the amount the vault ACTUALLY received (balance delta),
    /// so fee-on-transfer tokens cannot cause B3 to mint more than the
    /// vault holds. `b3Recipient` is opaque here; its semantics are defined
    /// by B3 consensus at the A3 activation.
    event Deposit(uint64 indexed depositId, address indexed token,
                  uint256 amount, bytes32 b3Recipient);

    event Released(address indexed token, address indexed to, uint256 amount);

    uint64 public nextDepositId;
    address public immutable releaseAuthority;

    error ZeroAuthority();
    error ZeroAmount();
    error NotAuthority();
    error TransferFailed();

    constructor(address _releaseAuthority) {
        if (_releaseAuthority == address(0)) revert ZeroAuthority();
        releaseAuthority = _releaseAuthority;
    }

    /// Lock native ETH.
    function depositETH(bytes32 b3Recipient) external payable {
        if (msg.value == 0) revert ZeroAmount();
        emit Deposit(nextDepositId++, address(0), msg.value, b3Recipient);
    }

    /// Lock an ERC-20. Requires prior approve(). Records the balance delta,
    /// not the requested amount.
    function depositToken(address token, uint256 amount, bytes32 b3Recipient) external {
        if (amount == 0) revert ZeroAmount();
        uint256 before = _balanceOf(token, address(this));
        _safeTransferFrom(token, msg.sender, address(this), amount);
        uint256 received = _balanceOf(token, address(this)) - before;
        if (received == 0) revert ZeroAmount();
        emit Deposit(nextDepositId++, token, received, b3Recipient);
    }

    /// Withdrawal path — only the release authority (the B3 finality
    /// verifier stack), which itself only pays out against an accepted B3
    /// finality certificate + withdrawal-tree proof (spec §5.2 + §6).
    function release(address token, address payable to, uint256 amount) external {
        if (msg.sender != releaseAuthority) revert NotAuthority();
        if (token == address(0)) {
            (bool ok, ) = to.call{value: amount}("");
            if (!ok) revert TransferFailed();
        } else {
            _safeTransfer(token, to, amount);
        }
        emit Released(token, to, amount);
    }

    // --- minimal safe-ERC20 (no library dependencies) ---------------------

    function _balanceOf(address token, address who) private view returns (uint256) {
        (bool ok, bytes memory data) =
            token.staticcall(abi.encodeWithSignature("balanceOf(address)", who));
        if (!ok || data.length < 32) revert TransferFailed();
        return abi.decode(data, (uint256));
    }

    function _safeTransferFrom(address token, address from, address to, uint256 amount) private {
        (bool ok, bytes memory data) = token.call(
            abi.encodeWithSignature("transferFrom(address,address,uint256)", from, to, amount));
        if (!ok || (data.length != 0 && !abi.decode(data, (bool)))) revert TransferFailed();
    }

    function _safeTransfer(address token, address to, uint256 amount) private {
        (bool ok, bytes memory data) = token.call(
            abi.encodeWithSignature("transfer(address,uint256)", to, amount));
        if (!ok || (data.length != 0 && !abi.decode(data, (bool)))) revert TransferFailed();
    }
}
