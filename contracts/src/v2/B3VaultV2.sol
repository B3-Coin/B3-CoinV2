// SPDX-License-Identifier: MIT
pragma solidity ^0.8.28;

import {B3TypesV2} from "./B3TypesV2.sol";
import {IB3FinalityVerifierV2} from "./IB3FinalityVerifierV2.sol";

/// The identity views a B-23 successor reads from the vault it replaces.
interface IB3VaultV2Identity {
    function B3_ASSET_ID() external view returns (bytes32);
    function ORIGIN_TOKEN() external view returns (address);
    function B3_CHAIN_DOMAIN() external view returns (bytes32);
    function ORIGIN_CHAIN_ID() external view returns (uint64);
}

/// Single-token, keyless Ethereum vault of bridge V2
/// (doc/design/b3-bridge-v2-contracts-spec.md §6).
///
/// No owner, admin, pause key, proxy, rescue key or fee switch. Verifier,
/// token, chain domain, AssetId, forwarder factory and per-deposit cap are
/// immutable and checked at construction.
///
/// - deposit (spec §6.2) keeps V1's ABI and V1's Deposit event byte for byte,
///   so B3's decoder needs only the new vault pin. It opens only while the
///   verifier's deposit gate is open and the token reports itself unpaused,
///   not deprecated and the vault not blacklisted. Only a codeless top-level
///   EOA or an F_d account of FORWARDER_FACTORY may call it (caller rule
///   C-13), which bounds the receipt B3 must parse.
/// - release (spec §6.4) pays a leaf V2 under any root the verifier ever
///   accepted at or above the leaf's B3 height (root history), so a frozen or
///   halted verifier keeps paying what it certified before it stopped. It
///   ignores verifier status and token flags. A failing or false-returning
///   transfer reverts the whole call, so the payment right survives.
/// - A B-23 migration needs no new function: B3 creates an ordinary leaf
///   naming this vault that pays the successor vault, and the successor's
///   absorb() counts the arrival in absorbedTotal, the only slot from which
///   B3 credits it.
/// - absorb (anyone) raises `locked` to the token balance. Plain transfers to
///   the vault create no B3 claim (open issue OI-6, architecture §20); absorb
///   only makes them visible as reserve.
///
/// Nothing is ever refunded once tokens are in the vault. The storage layout
/// is normative (spec §6.6) because B3 proves slots 0, 1 and 3 by EIP-1186:
/// do not reorder state variables or add inheritance with storage. The
/// reentrancy lock is transient and occupies no storage slot.
contract B3VaultV2 is IB3VaultV2Identity {
    struct Config {
        IB3FinalityVerifierV2 verifier;
        bytes32 verifierCodeHash;
        bytes32 b3ChainDomain;
        address originToken;
        bytes32 tokenCodeHash;
        address forwarderFactory; // F_d factory; may be a predicted, still codeless address
        uint256 maxDepositRaw;
        address predecessor; // zero for the launch vault; the retiring vault for a B-23 successor
        bytes32 predecessorAssetId; // zero for the launch vault; predecessor.B3_ASSET_ID() otherwise
    }

    bytes32 public constant LEAF_TAG_V2 = B3TypesV2.LEAF_TAG_V2;

    bytes4 private constant PAUSED_SELECTOR = bytes4(keccak256("paused()"));
    bytes4 private constant DEPRECATED_SELECTOR = bytes4(keccak256("deprecated()"));
    bytes4 private constant BLACKLISTED_SELECTOR = bytes4(keccak256("isBlackListed(address)"));
    bytes4 private constant IS_ACCOUNT_SELECTOR = bytes4(keccak256("isAccount(address)"));

    IB3FinalityVerifierV2 public immutable VERIFIER;
    bytes32 public immutable VERIFIER_CODE_HASH;
    bytes32 public immutable B3_CHAIN_DOMAIN;
    address public immutable ORIGIN_TOKEN;
    bytes32 public immutable TOKEN_CODE_HASH;
    address public immutable FORWARDER_FACTORY;
    uint256 public immutable MAX_DEPOSIT_RAW;
    uint64 public immutable BRIDGE_ACTIVATION_HEIGHT;
    uint64 public immutable ORIGIN_CHAIN_ID;
    bytes32 public immutable B3_ASSET_ID;
    address public immutable PREDECESSOR;

    // ------------------------------------------------ normative slots (§6.6)
    uint64 public nextDepositId; // slot 0
    uint256 public locked; // slot 1
    mapping(uint64 withdrawalId => bool) public released; // slot 2
    uint256 public absorbedTotal; // slot 3: lifetime sum absorb() added to locked

    bool private transient _entered;

    /// V1's event (contracts/src/B3StakerBridge.sol), unchanged.
    event Deposit(uint64 indexed depositId, address indexed token, uint256 amount, bytes32 b3Recipient);
    /// V1's event (contracts/src/B3StakerBridge.sol), unchanged.
    event Withdrawn(
        uint64 indexed withdrawalId, address indexed token, address indexed recipient, uint256 amount, uint64 b3Height
    );
    event Absorbed(uint256 increase, uint256 absorbedTotal);

    error BadConfig();
    error Reentrancy();
    error DepositClosed();
    error BadCaller();
    error ZeroAmount();
    error BadB3Recipient();
    error DepositCapExceeded();
    error TransferFailed();
    error WithdrawalIdOutOfRange();
    error WithdrawalBeforeActivation();
    error ZeroRecipient();
    error AlreadyReleased();
    error UnknownRoot();
    error RootBelowLeafHeight();
    error InsufficientReserve();

    modifier nonReentrant() {
        if (_entered) revert Reentrancy();
        _entered = true;
        _;
        _entered = false;
    }

    constructor(Config memory cfg) {
        address verifier = address(cfg.verifier);
        if (
            verifier.code.length == 0 || cfg.verifierCodeHash == bytes32(0) || verifier.codehash != cfg.verifierCodeHash
                || cfg.b3ChainDomain == bytes32(0) || cfg.originToken.code.length == 0
                || cfg.tokenCodeHash == bytes32(0) || cfg.originToken.codehash != cfg.tokenCodeHash
                || cfg.forwarderFactory == address(0) || cfg.maxDepositRaw == 0 || block.chainid > type(uint64).max
        ) revert BadConfig();

        (bool ok, bytes32 word) = _staticWord(verifier, abi.encodeCall(IB3FinalityVerifierV2.CHAIN_DOMAIN, ()));
        if (!ok || word != cfg.b3ChainDomain) revert BadConfig();
        (ok, word) = _staticWord(verifier, abi.encodeCall(IB3FinalityVerifierV2.BRIDGE_ACTIVATION_HEIGHT, ()));
        if (!ok || word == bytes32(0) || uint256(word) > type(uint64).max) revert BadConfig();
        uint64 activationHeight = uint64(uint256(word));

        bytes32 assetId;
        if (cfg.predecessor == address(0)) {
            if (cfg.predecessorAssetId != bytes32(0)) revert BadConfig();
            assetId = B3TypesV2.assetId(cfg.b3ChainDomain, uint64(block.chainid), address(this), cfg.originToken);
        } else {
            // B-23 successor: same chain, token, chain domain and AssetId as the
            // vault it replaces, which the migrating hard fork pins.
            if (cfg.predecessor.code.length == 0 || cfg.predecessorAssetId == bytes32(0)) revert BadConfig();
            (ok, word) = _staticWord(cfg.predecessor, abi.encodeCall(IB3VaultV2Identity.B3_ASSET_ID, ()));
            if (!ok || word != cfg.predecessorAssetId) revert BadConfig();
            (ok, word) = _staticWord(cfg.predecessor, abi.encodeCall(IB3VaultV2Identity.ORIGIN_TOKEN, ()));
            if (!ok || word != bytes32(uint256(uint160(cfg.originToken)))) revert BadConfig();
            (ok, word) = _staticWord(cfg.predecessor, abi.encodeCall(IB3VaultV2Identity.B3_CHAIN_DOMAIN, ()));
            if (!ok || word != cfg.b3ChainDomain) revert BadConfig();
            (ok, word) = _staticWord(cfg.predecessor, abi.encodeCall(IB3VaultV2Identity.ORIGIN_CHAIN_ID, ()));
            if (!ok || word != bytes32(block.chainid)) revert BadConfig();
            assetId = cfg.predecessorAssetId;
        }

        VERIFIER = cfg.verifier;
        VERIFIER_CODE_HASH = cfg.verifierCodeHash;
        B3_CHAIN_DOMAIN = cfg.b3ChainDomain;
        ORIGIN_TOKEN = cfg.originToken;
        TOKEN_CODE_HASH = cfg.tokenCodeHash;
        FORWARDER_FACTORY = cfg.forwarderFactory;
        MAX_DEPOSIT_RAW = cfg.maxDepositRaw;
        BRIDGE_ACTIVATION_HEIGHT = activationHeight;
        ORIGIN_CHAIN_ID = uint64(block.chainid);
        B3_ASSET_ID = assetId;
        PREDECESSOR = cfg.predecessor;
    }

    // ================================================================= deposit

    /// Locks `amount` of ORIGIN_TOKEN for `b3Recipient` (RECIPIENT_V1). The
    /// event carries the received balance delta, so a fee-charging token is
    /// accounted exactly; one Deposit is one indivisible B3 mint.
    function deposit(uint256 amount, bytes32 b3Recipient) external nonReentrant {
        // 1. Gate: verifier deposit window and token status (spec §6.2 step 1).
        if (!_gateOpen()) revert DepositClosed();
        // 2. Caller rule (C-13): a codeless top-level EOA, or an F_d account.
        // Contract wallets, ERC-4337 bundles and EIP-7702 accounts (which
        // carry a delegation designator as code) fund their F_d by transfer.
        if (!(msg.sender == tx.origin && msg.sender.code.length == 0) && !_isForwarderAccount(msg.sender)) {
            revert BadCaller();
        }
        // 3. RECIPIENT_V1 is exactly 11 zero bytes, version 1, then a non-zero
        // 20-byte B3 P2PKH key hash (doc/design/b3-bridge-recipient-encoding.md).
        // Anything else is unmintable and would strand the deposit.
        if (amount == 0) revert ZeroAmount();
        uint256 encodedRecipient = uint256(b3Recipient);
        if (encodedRecipient >> 160 != 1 || (encodedRecipient & type(uint160).max) == 0) {
            revert BadB3Recipient();
        }
        // 4. Pull by balance delta.
        uint256 beforeBalance = _balanceOf(address(this));
        _safeTransferFrom(msg.sender, address(this), amount);
        uint256 received = _balanceOf(address(this)) - beforeBalance;
        if (received == 0) revert ZeroAmount();
        if (received > MAX_DEPOSIT_RAW) revert DepositCapExceeded();
        locked += received;
        // 5. V1's event, unchanged.
        emit Deposit(nextDepositId++, ORIGIN_TOKEN, received, b3Recipient);
    }

    /// The deposit gate alone (verifier window and token status), without the
    /// caller rule. Wallets read it before asking for an approval.
    function depositGateOpen() external view returns (bool) {
        return _gateOpen();
    }

    // ================================================================= release

    /// Permissionless release of one leaf V2 (spec §6.4) under any root the
    /// verifier accepted at or above `w.b3Height`. Verifier status and token
    /// flags are deliberately not consulted.
    function release(B3TypesV2.WithdrawalV2 calldata w, bytes32[32] calldata path) external nonReentrant {
        if (w.withdrawalId >= B3TypesV2.MAX_INDEX) revert WithdrawalIdOutOfRange();
        if (w.b3Height < BRIDGE_ACTIVATION_HEIGHT) revert WithdrawalBeforeActivation();
        if (w.recipient == address(0)) revert ZeroRecipient();
        if (w.amount == 0) revert ZeroAmount();
        if (released[w.withdrawalId]) revert AlreadyReleased();

        bytes32 root = B3TypesV2.rootFromPath(withdrawalLeaf(w), w.withdrawalId, path);
        uint64 rootHeight = VERIFIER.acceptedRootHeight(root);
        if (rootHeight == 0) revert UnknownRoot();
        if (rootHeight < w.b3Height) revert RootBelowLeafHeight();
        if (w.amount > locked) revert InsufficientReserve();

        released[w.withdrawalId] = true;
        locked -= w.amount;
        // A reverting or false-returning transfer reverts everything above, so
        // released[id] stays unset and the leaf remains payable.
        _safeTransfer(w.recipient, w.amount);
        emit Withdrawn(w.withdrawalId, ORIGIN_TOKEN, w.recipient, w.amount, w.b3Height);
    }

    /// Leaf V2: keccak256 of the 180-byte preimage LEAF_TAG_V2 ‖ B3_ASSET_ID ‖
    /// u64 id ‖ u64 chain id ‖ this vault ‖ ORIGIN_TOKEN ‖ recipient ‖
    /// u256 amount ‖ u64 b3Height. A successor computes a different leaf for
    /// the same id because it names itself.
    function withdrawalLeaf(B3TypesV2.WithdrawalV2 calldata w) public view returns (bytes32) {
        return B3TypesV2.withdrawalLeaf(
            B3_ASSET_ID,
            w.withdrawalId,
            uint64(block.chainid),
            address(this),
            ORIGIN_TOKEN,
            w.recipient,
            w.amount,
            w.b3Height
        );
    }

    // ================================================================== absorb

    /// Sets locked = max(locked, token balance) and adds the increase to
    /// absorbedTotal. A successor calls it after a migration leaf paid it; B3
    /// credits a migration only from absorbedTotal (node changes §7.6).
    /// Stray transfers it counts create no B3 claim.
    function absorb() external nonReentrant returns (uint256 increase) {
        uint256 balance = _balanceOf(address(this));
        uint256 current = locked;
        if (balance <= current) return 0;
        increase = balance - current;
        locked = balance;
        uint256 total = absorbedTotal + increase;
        absorbedTotal = total;
        emit Absorbed(increase, total);
    }

    // ================================================================ internal

    function _gateOpen() private view returns (bool) {
        return VERIFIER.depositsOpen() && _tokenFlagClear(PAUSED_SELECTOR, false)
            && _tokenFlagClear(DEPRECATED_SELECTOR, false) && _tokenFlagClear(BLACKLISTED_SELECTOR, true);
    }

    /// True only if the token view answers with exactly 32 bytes, all zero
    /// (false). A missing function, a revert or any other answer closes the gate.
    function _tokenFlagClear(bytes4 selector, bool withVault) private view returns (bool clear) {
        address token = ORIGIN_TOKEN;
        assembly ("memory-safe") {
            mstore(0x00, selector)
            mstore(0x04, address())
            let ok := staticcall(gas(), token, 0x00, add(4, mul(withVault, 0x20)), 0x00, 0x20)
            clear := and(ok, and(eq(returndatasize(), 0x20), iszero(mload(0x00))))
        }
    }

    /// FORWARDER_FACTORY.isAccount(account) == true, read by a low-level call:
    /// a codeless (predicted) factory, a revert or any answer other than 32
    /// bytes equal to 1 refuses the caller.
    function _isForwarderAccount(address account) private view returns (bool yes) {
        address factory = FORWARDER_FACTORY;
        bytes4 selector = IS_ACCOUNT_SELECTOR;
        assembly ("memory-safe") {
            mstore(0x00, selector)
            mstore(0x04, account)
            let ok := staticcall(gas(), factory, 0x00, 0x24, 0x00, 0x20)
            yes := and(ok, and(eq(returndatasize(), 0x20), eq(mload(0x00), 1)))
        }
    }

    function _balanceOf(address account) private view returns (uint256) {
        (bool ok, bytes memory data) = ORIGIN_TOKEN.staticcall(abi.encodeWithSignature("balanceOf(address)", account));
        if (!ok || data.length != 32) revert TransferFailed();
        return abi.decode(data, (uint256));
    }

    /// Accepts an empty or `true` return (USDT returns nothing).
    function _safeTransferFrom(address from, address to, uint256 amount) private {
        (bool ok, bytes memory data) =
            ORIGIN_TOKEN.call(abi.encodeWithSignature("transferFrom(address,address,uint256)", from, to, amount));
        if (!ok || (data.length != 0 && (data.length != 32 || !abi.decode(data, (bool))))) {
            revert TransferFailed();
        }
    }

    /// Accepts an empty or `true` return (USDT returns nothing).
    function _safeTransfer(address to, uint256 amount) private {
        (bool ok, bytes memory data) =
            ORIGIN_TOKEN.call(abi.encodeWithSignature("transfer(address,uint256)", to, amount));
        if (!ok || (data.length != 0 && (data.length != 32 || !abi.decode(data, (bool))))) {
            revert TransferFailed();
        }
    }

    /// One 32-byte answer of a view, or ok = false (constructor reads only).
    function _staticWord(address target, bytes memory callData) private view returns (bool ok, bytes32 word) {
        bytes memory data;
        (ok, data) = target.staticcall(callData);
        if (!ok || data.length != 32) return (false, bytes32(0));
        word = abi.decode(data, (bytes32));
    }
}
