// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {B3FinalityVerifier} from "./B3FinalityVerifier.sol";

/// Single-asset, keyless Ethereum vault controlled only by B3 staker finality.
///
/// The token, B3 AssetId, origin chain, verifier runtime, and B3 bridge height
/// are immutable. There is no owner, rescue key, arbitrary token selector, or
/// upgrade path. Anyone may relay a withdrawal, but funds move only when its
/// exact leaf is in a withdrawal root signed by the configured stake/BLS set.
contract B3StakerBridge {
    struct Withdrawal {
        uint64 withdrawalId;
        address recipient;
        uint256 amount;
        uint64 b3Height;
    }

    B3FinalityVerifier public immutable verifier;
    bytes32 public immutable VERIFIER_CODE_HASH;
    uint64 public immutable ORIGIN_CHAIN_ID;
    bytes32 public immutable B3_ASSET_ID;
    address public immutable ORIGIN_TOKEN;
    uint64 public immutable BRIDGE_ACTIVATION_HEIGHT;

    uint64 public nextDepositId;
    uint256 public locked;
    mapping(uint64 => bool) public released;

    event Deposit(
        uint64 indexed depositId,
        address indexed token,
        uint256 amount,
        bytes32 b3Recipient
    );
    event Withdrawn(
        uint64 indexed withdrawalId,
        address indexed token,
        address indexed recipient,
        uint256 amount,
        uint64 b3Height
    );

    error BadBootstrap();
    error ZeroAmount();
    error ZeroRecipient();
    error TransferFailed();
    error Reentrancy();
    error WithdrawalIdOutOfRange();
    error BridgeNotReady();
    error WithdrawalBeforeActivation();
    error WithdrawalNotFinalized();
    error AlreadyReleased();
    error BadWithdrawalProof();
    error InsufficientReserve();

    uint256 private _entered;
    modifier nonReentrant() {
        if (_entered != 0) revert Reentrancy();
        _entered = 1;
        _;
        _entered = 0;
    }

    constructor(
        B3FinalityVerifier verifier_,
        bytes32 expectedVerifierCodeHash,
        bytes32 b3AssetId,
        address originToken
    ) {
        if (
            address(verifier_) == address(0) ||
            address(verifier_).code.length == 0 ||
            expectedVerifierCodeHash == bytes32(0) ||
            _codeHash(address(verifier_)) != expectedVerifierCodeHash ||
            b3AssetId == bytes32(0) ||
            originToken == address(0) ||
            originToken.code.length == 0 ||
            block.chainid > type(uint64).max
        ) revert BadBootstrap();

        verifier = verifier_;
        VERIFIER_CODE_HASH = expectedVerifierCodeHash;
        ORIGIN_CHAIN_ID = uint64(block.chainid);
        B3_ASSET_ID = b3AssetId;
        ORIGIN_TOKEN = originToken;
        BRIDGE_ACTIVATION_HEIGHT = verifier_.BRIDGE_ACTIVATION_HEIGHT();
    }

    /// Lock only the one configured origin token. The emitted amount is the
    /// actual balance delta, preserving exact reserve accounting for USDT-style
    /// no-return tokens and rejecting zero-received transfers.
    function deposit(uint256 amount, bytes32 b3Recipient)
        external
        nonReentrant
    {
        if (!verifier.bridgeReady()) revert BridgeNotReady();
        if (amount == 0) revert ZeroAmount();
        uint256 beforeBalance = _balanceOf(address(this));
        _safeTransferFrom(msg.sender, address(this), amount);
        uint256 received = _balanceOf(address(this)) - beforeBalance;
        if (received == 0) revert ZeroAmount();
        locked += received;
        emit Deposit(nextDepositId++, ORIGIN_TOKEN, received, b3Recipient);
    }

    /// Permissionless release from the cumulative, ordered depth-32 B3 tree.
    /// Leaf preimage is exactly 128 bytes:
    /// u64 id || u64 chain || bytes32 asset || address token || address recipient
    /// || u256 amount || u64 B3 height. The older document's 164-byte label was
    /// an arithmetic typo; no undocumented padding is inserted.
    function release(Withdrawal calldata withdrawal, bytes32[32] calldata path)
        external
        nonReentrant
    {
        if (!verifier.bridgeReady()) revert BridgeNotReady();
        if (withdrawal.withdrawalId >= (uint64(1) << 32)) {
            revert WithdrawalIdOutOfRange();
        }
        if (withdrawal.b3Height < BRIDGE_ACTIVATION_HEIGHT) {
            revert WithdrawalBeforeActivation();
        }
        if (withdrawal.recipient == address(0)) revert ZeroRecipient();
        if (withdrawal.amount == 0) revert ZeroAmount();
        if (released[withdrawal.withdrawalId]) revert AlreadyReleased();
        if (verifier.latestBridgeFinalizedHeight() < withdrawal.b3Height) {
            revert WithdrawalNotFinalized();
        }

        bytes32 node = withdrawalLeaf(withdrawal);
        uint256 index = withdrawal.withdrawalId;
        for (uint256 level = 0; level < 32; ++level) {
            node = ((index >> level) & 1) == 0
                ? keccak256(abi.encodePacked(node, path[level]))
                : keccak256(abi.encodePacked(path[level], node));
        }
        if (node != verifier.latestBridgeWithdrawalRoot()) {
            revert BadWithdrawalProof();
        }
        if (withdrawal.amount > locked) revert InsufficientReserve();

        released[withdrawal.withdrawalId] = true;
        locked -= withdrawal.amount;
        _safeTransfer(withdrawal.recipient, withdrawal.amount);
        emit Withdrawn(
            withdrawal.withdrawalId,
            ORIGIN_TOKEN,
            withdrawal.recipient,
            withdrawal.amount,
            withdrawal.b3Height
        );
    }

    function withdrawalLeaf(Withdrawal calldata withdrawal)
        public
        view
        returns (bytes32)
    {
        return keccak256(
            abi.encodePacked(
                withdrawal.withdrawalId,
                ORIGIN_CHAIN_ID,
                B3_ASSET_ID,
                ORIGIN_TOKEN,
                withdrawal.recipient,
                withdrawal.amount,
                withdrawal.b3Height
            )
        );
    }

    function _balanceOf(address account) private view returns (uint256) {
        (bool ok, bytes memory data) = ORIGIN_TOKEN.staticcall(
            abi.encodeWithSignature("balanceOf(address)", account)
        );
        if (!ok || data.length != 32) revert TransferFailed();
        return abi.decode(data, (uint256));
    }

    function _safeTransferFrom(address from, address to, uint256 amount) private {
        (bool ok, bytes memory data) = ORIGIN_TOKEN.call(
            abi.encodeWithSignature(
                "transferFrom(address,address,uint256)",
                from,
                to,
                amount
            )
        );
        if (!ok || (data.length != 0 && (data.length != 32 || !abi.decode(data, (bool))))) {
            revert TransferFailed();
        }
    }

    function _safeTransfer(address to, uint256 amount) private {
        (bool ok, bytes memory data) = ORIGIN_TOKEN.call(
            abi.encodeWithSignature("transfer(address,uint256)", to, amount)
        );
        if (!ok || (data.length != 0 && (data.length != 32 || !abi.decode(data, (bool))))) {
            revert TransferFailed();
        }
    }

    function _codeHash(address account) private view returns (bytes32 hash) {
        assembly {
            hash := extcodehash(account)
        }
    }
}
