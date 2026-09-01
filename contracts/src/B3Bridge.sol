// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {B3FinalityVerifier} from "./B3FinalityVerifier.sol";

/// @title B3Bridge — trust-minimized withdrawals (deposits + §6 release).
///
/// The DECENTRALIZED bridge: funds leave ONLY on a Merkle proof that a
/// BRIDGE_BURN on B3 is included in `verifier.latest().withdrawalRoot`, which
/// the B3 validator committee attested via BLS (B3FinalityVerifier §5). There
/// is no owner, no release authority, no admin. Deposits mirror the managed
/// vault's hardened accounting (balance-delta, USDT-safe, reentrancy-locked)
/// so the same B3-side proof pipeline (bridge/deposit.h) consumes the events.
///
/// Withdrawal leaf and path are doc/design/b3-cross-chain-finality-v1.md §6
/// EXACTLY (WITHDRAWAL_TREE_DEPTH = 32, keccak nodes, 164-byte leaf preimage).
contract B3Bridge {
    struct Withdrawal {
        uint64  withdrawalId;
        uint64  originChainId;    // B3 chain id embedded in the leaf
        bytes32 assetId;          // B3-side asset id
        address originToken;      // Ethereum token (address(0) = ETH)
        address recipient;
        uint256 amount;
        uint64  b3Height;
    }

    B3FinalityVerifier public immutable verifier;
    uint256 public immutable ETH_CHAIN_ID;      // this contract's block.chainid, pinned at deploy

    // deposit-leg liabilities per token (address(0) = ETH); rescue never applies here.
    mapping(address => uint256) public locked;
    uint64 public nextDepositId;
    mapping(uint64 => bool) public released;    // §6 double-spend guard

    event Deposit(uint64 indexed depositId, address indexed token,
                  uint256 amount, bytes32 b3Recipient);
    event Withdrawn(uint64 indexed withdrawalId, address indexed token,
                    address indexed to, uint256 amount);

    error ZeroAmount();
    error TransferFailed();
    error Reentrancy();
    error WrongChain();
    error AlreadyReleased();
    error BadWithdrawalProof();

    uint256 private _entered;
    modifier nonReentrant() {
        if (_entered != 0) revert Reentrancy();
        _entered = 1; _; _entered = 0;
    }

    constructor(B3FinalityVerifier verifier_) {
        verifier = verifier_;
        ETH_CHAIN_ID = block.chainid;
    }

    // --- deposit leg (same hardened accounting as the managed vault) ------
    function depositETH(bytes32 b3Recipient) external payable nonReentrant {
        if (msg.value == 0) revert ZeroAmount();
        locked[address(0)] += msg.value;
        emit Deposit(nextDepositId++, address(0), msg.value, b3Recipient);
    }

    function depositToken(address token, uint256 amount, bytes32 b3Recipient) external nonReentrant {
        if (amount == 0) revert ZeroAmount();
        uint256 pre = _balanceOf(token, address(this));
        _safeTransferFrom(token, msg.sender, address(this), amount);
        uint256 received = _balanceOf(token, address(this)) - pre;
        if (received == 0) revert ZeroAmount();
        locked[token] += received;
        emit Deposit(nextDepositId++, token, received, b3Recipient);
    }

    // --- withdrawal leg (spec §6) -----------------------------------------
    /// Release `w` iff its §6 leaf proves into the verifier's latest
    /// withdrawal root along `path` (a 32-level sibling path indexed by
    /// withdrawalId). Cumulative tree ⇒ any burn proves against any later root.
    function release(Withdrawal calldata w, bytes32[32] calldata path) external nonReentrant {
        if (w.originChainId != ETH_CHAIN_ID) revert WrongChain();
        if (released[w.withdrawalId]) revert AlreadyReleased();

        bytes32 node = keccak256(abi.encodePacked(
            w.withdrawalId, w.originChainId, w.assetId, w.originToken,
            w.recipient, w.amount, w.b3Height));           // 164-byte preimage
        uint256 idx = w.withdrawalId;
        for (uint256 k = 0; k < 32; k++) {
            node = (idx >> k) & 1 == 0
                ? keccak256(abi.encodePacked(node, path[k]))
                : keccak256(abi.encodePacked(path[k], node));
        }
        if (node != verifier.latestWithdrawalRoot()) revert BadWithdrawalProof();

        released[w.withdrawalId] = true;
        if (w.amount != 0) {
            locked[w.originToken] = w.amount >= locked[w.originToken]
                ? 0 : locked[w.originToken] - w.amount;    // saturating accounting only
            if (w.originToken == address(0)) {
                (bool ok, ) = payable(w.recipient).call{value: w.amount}("");
                if (!ok) revert TransferFailed();
            } else {
                _safeTransfer(w.originToken, w.recipient, w.amount);
            }
        }
        emit Withdrawn(w.withdrawalId, w.originToken, w.recipient, w.amount);
    }

    // --- minimal safe-ERC20 (USDT-tolerant, no libraries) -----------------
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
