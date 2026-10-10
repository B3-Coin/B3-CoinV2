// SPDX-License-Identifier: MIT
pragma solidity ^0.8.28;

import {B3Forwarder} from "./B3Forwarder.sol";
import {B3ForwarderTypesV2, IForwarderTokenV2} from "./B3ForwarderTypesV2.sol";

/// The Vault V2 surface the deposit forwarder factory reads and calls.
interface IB3VaultV2Forwarding {
    function FORWARDER_FACTORY() external view returns (address);
    function ORIGIN_TOKEN() external view returns (address);
    function MAX_DEPOSIT_RAW() external view returns (uint256);
    function deposit(uint256 amount, bytes32 b3Recipient) external;
}

/// Factory of deposit forwarders F_d for one Vault V2
/// (doc/design/b3-bridge-v2-contracts-spec.md §7; route (b)).
///
/// An F_d account is the CREATE2 address of B3Forwarder for
/// B3ForwarderTypesV2.depositSalt(p). USDT arrives there by plain transfer
/// (the USDT0 OFT adapter's credit from Arbitrum, or a user's own transfer),
/// before or after the account is deployed. A flush moves it into the vault
/// with one vault.deposit made by the account, which the vault admits as a
/// caller because isAccount(account) is true (caller rule C-13).
///
/// No owner, admin or pause. Fees are capped by the user: p.feeCap bounds the
/// lifetime sum of fees charged to the account (D5), and the caller of a flush
/// picks only the fee, never the amount, so a balance cannot be split into
/// many fee-paying flushes. Accrued fees stay in the account (`reserved`)
/// until collectFee pays them, so refunds never touch them.
///
/// Rules:
/// - PR-1 / D1: flush makes exactly one vault.deposit and emits nothing of its
///   own; its receipt carries only the token Transfer and the vault Deposit.
/// - PR-2: flush runs only as the top-level call of a codeless EOA
///   (msg.sender == tx.origin, no code; EIP-7702 accounts carry code), so no
///   wrapper can add logs to the receipt B3 parses.
/// - Escapes: refund (p.refundEOA, any time, any p) and returnToOwner (anyone,
///   after p.notAfter, pays p.refundEOA).
///
/// Invariants (spec §7.4): D2 tokens leave an account only to the vault, a
/// payee's accrued fees, or as refundEOA directs; D3 balance >= reserved
/// unless the token itself takes funds (blacklist burn), hence the saturating
/// freeBalance; D4 a flush and a refund never spend the same unit; D5
/// feesCharged <= p.feeCap; D6 a flush deposits min(free - fee, MAX_DEPOSIT_RAW).
///
/// No NUMBER opcode: time is block.timestamp only. The reentrancy lock is
/// transient.
contract B3DepositForwarderFactory {
    /// Smallest amount a flush deposits (1 USDT); smaller balances are refunded.
    uint256 public constant MIN_FLUSH_RAW = 1_000_000;

    address public immutable VAULT;
    /// The vault's ORIGIN_TOKEN.
    address public immutable TOKEN;
    /// The vault's per-deposit cap; a flush deposits at most this much.
    uint256 public immutable MAX_DEPOSIT_RAW;
    /// keccak256(type(B3Forwarder).creationCode); the manifest pin B3 derives with.
    bytes32 public immutable FORWARDER_INIT_CODE_HASH;

    /// Accounts this factory deployed; Vault V2's caller rule reads it.
    mapping(address account => bool) public isAccount;
    /// Fees accrued in an account and not yet collected.
    mapping(address account => uint256) public reserved;
    /// Lifetime fees charged to an account (D5: never above its p.feeCap).
    mapping(address account => uint256) public feesCharged;
    /// Fees an account owes each payee.
    mapping(address account => mapping(address payee => uint256)) public owed;

    bool private transient _entered;

    /// p is public, from deploy or publish (relayers read it, spec §7.4).
    event Published(address indexed account, B3ForwarderTypesV2.DepositParams p);
    event FeeCollected(address indexed account, address indexed payee, uint256 amount);
    /// token == address(0) is native ETH.
    event Refunded(address indexed account, address indexed token, address indexed to, uint256 amount);
    event ReturnedToOwner(address indexed account, address indexed refundEOA, uint256 amount, uint256 fee);

    error BadConfig();
    error Reentrancy();
    error BadCaller();
    error WrongVault();
    error BadB3Recipient();
    error BadRefundEOA();
    error AlreadyDeployed();
    error NotDeployed();
    error DeployFailed();
    error Expired();
    error NotExpired();
    error FeeCapExceeded();
    error InsufficientBalance();
    error BelowMinimum();
    error NotRefundEOA();
    error BadRefundTarget();
    error ZeroAmount();
    error NothingOwed();
    error NothingToReturn();
    error NoCode(address target);
    error TokenCallFailed();

    modifier nonReentrant() {
        if (_entered) revert Reentrancy();
        _entered = true;
        _;
        _entered = false;
    }

    /// `vault` must already exist with code hash `vaultCodeHash` and name this
    /// factory as its FORWARDER_FACTORY (the deployer predicted this address).
    constructor(address vault, bytes32 vaultCodeHash) {
        if (vault.code.length == 0 || vaultCodeHash == bytes32(0) || vault.codehash != vaultCodeHash) {
            revert BadConfig();
        }
        (bool ok, bytes32 word) = _staticWord(vault, abi.encodeCall(IB3VaultV2Forwarding.FORWARDER_FACTORY, ()));
        if (!ok || word != bytes32(uint256(uint160(address(this))))) revert BadConfig();
        (ok, word) = _staticWord(vault, abi.encodeCall(IB3VaultV2Forwarding.ORIGIN_TOKEN, ()));
        if (!ok || uint256(word) >> 160 != 0) revert BadConfig();
        address token = address(uint160(uint256(word)));
        if (token.code.length == 0) revert BadConfig();
        (ok, word) = _staticWord(vault, abi.encodeCall(IB3VaultV2Forwarding.MAX_DEPOSIT_RAW, ()));
        if (!ok || uint256(word) < MIN_FLUSH_RAW) revert BadConfig();

        VAULT = vault;
        TOKEN = token;
        MAX_DEPOSIT_RAW = uint256(word);
        FORWARDER_INIT_CODE_HASH = keccak256(type(B3Forwarder).creationCode);
    }

    // ================================================================== views

    /// The F_d address of `p`, deployed or not. Defined for every p.
    function accountOf(B3ForwarderTypesV2.DepositParams calldata p) public view returns (address) {
        bytes32 salt = B3ForwarderTypesV2.depositSalt(p);
        return B3ForwarderTypesV2.create2Address(address(this), salt, FORWARDER_INIT_CODE_HASH);
    }

    /// p.vault is VAULT, p.b3Recipient is RECIPIENT_V1 and p.refundEOA is
    /// non-zero and none of the vault, the token or this factory (OI-6: a
    /// returnToOwner must never pay a contract that gives no claim back).
    function isValid(B3ForwarderTypesV2.DepositParams calldata p) public view returns (bool) {
        return p.vault == VAULT && B3ForwarderTypesV2.recipientV1Valid(p.b3Recipient) && _refundEOAValid(p.refundEOA);
    }

    /// The account's token balance not reserved for accrued fees, or 0.
    function freeBalance(address account) public view returns (uint256) {
        uint256 balance = _balanceOf(account);
        uint256 held = reserved[account];
        return balance > held ? balance - held : 0;
    }

    // ============================================================== lifecycle

    /// Deploys the account of a valid `p` (anyone), approves the vault for
    /// the token once, and publishes p. Reverts AlreadyDeployed for an
    /// existing account.
    function deploy(B3ForwarderTypesV2.DepositParams calldata p) external nonReentrant returns (address account) {
        _requireValid(p);
        account = accountOf(p);
        if (isAccount[account]) revert AlreadyDeployed();
        _deploy(p, account);
    }

    /// Publishes a valid `p` (anyone) without deploying it; no state.
    function publish(B3ForwarderTypesV2.DepositParams calldata p) external returns (address account) {
        _requireValid(p);
        account = accountOf(p);
        emit Published(account, p);
    }

    /// Deposits min(free - fee, MAX_DEPOSIT_RAW) of the account into the vault
    /// for p.b3Recipient and accrues `fee` to the caller, within p.feeCap.
    /// Only a codeless EOA's top-level call (PR-2), only for a deployed
    /// account, only until p.notAfter. Emits nothing (PR-1). A vault revert
    /// (DepositClosed while the gate is shut) bubbles and undoes the accrual.
    function flush(B3ForwarderTypesV2.DepositParams calldata p, uint256 fee)
        external
        nonReentrant
        returns (uint256 amount)
    {
        if (msg.sender != tx.origin || tx.origin.code.length != 0) revert BadCaller();
        _requireValid(p);
        address account = accountOf(p);
        if (!isAccount[account]) revert NotDeployed();
        if (block.timestamp > p.notAfter) revert Expired();

        uint256 available = _afterFee(account, p.feeCap, fee);
        amount = available < MAX_DEPOSIT_RAW ? available : MAX_DEPOSIT_RAW;
        if (amount < MIN_FLUSH_RAW) revert BelowMinimum();

        _accrue(account, fee, msg.sender);
        B3Forwarder(account).exec(VAULT, 0, abi.encodeCall(IB3VaultV2Forwarding.deposit, (amount, p.b3Recipient)));
    }

    /// Pays `payee` the fees `account` owes it (anyone may trigger it).
    function collectFee(address account, address payee) external nonReentrant returns (uint256 amount) {
        amount = owed[account][payee];
        if (amount == 0) revert NothingOwed();
        owed[account][payee] = 0;
        reserved[account] -= amount;
        _tokenCall(account, TOKEN, abi.encodeCall(IForwarderTokenV2.transfer, (payee, amount)));
        emit FeeCollected(account, payee, amount);
    }

    /// Sends `amount` of `token` (address(0): native ETH) from the account of
    /// `p` to `to`. Only p.refundEOA; any p, valid or not, at any time,
    /// deposits open or closed; deploys the account on demand. For TOKEN at
    /// most the free balance, so accrued fees stay payable. `to` is never the
    /// vault (a plain transfer there gives no claim, OI-6), the account or
    /// this factory.
    function refund(B3ForwarderTypesV2.DepositParams calldata p, address token, address to, uint256 amount)
        external
        nonReentrant
    {
        if (msg.sender != p.refundEOA) revert NotRefundEOA();
        address account = accountOf(p);
        if (to == address(0) || to == VAULT || to == account || to == address(this)) revert BadRefundTarget();
        if (amount == 0) revert ZeroAmount();
        if (!isAccount[account]) _deploy(p, account);

        if (token == address(0)) {
            if (amount > account.balance) revert InsufficientBalance();
            B3Forwarder(account).exec(to, amount, "");
        } else {
            if (token == TOKEN) {
                if (amount > freeBalance(account)) revert InsufficientBalance();
            } else if (token.code.length == 0) {
                revert NoCode(token);
            }
            _tokenCall(account, token, abi.encodeCall(IForwarderTokenV2.transfer, (to, amount)));
        }
        emit Refunded(account, token, to, amount);
    }

    /// After p.notAfter, pays the free balance less `fee` to p.refundEOA and
    /// accrues `fee` to the caller within p.feeCap (anyone), so a user with no
    /// Ethereum ETH is not stuck. Deploys the account on demand.
    function returnToOwner(B3ForwarderTypesV2.DepositParams calldata p, uint256 fee)
        external
        nonReentrant
        returns (uint256 amount)
    {
        _requireValid(p);
        if (block.timestamp <= p.notAfter) revert NotExpired();
        address account = accountOf(p);
        if (!isAccount[account]) _deploy(p, account);

        amount = _afterFee(account, p.feeCap, fee);
        if (amount == 0) revert NothingToReturn();
        _accrue(account, fee, msg.sender);
        _tokenCall(account, TOKEN, abi.encodeCall(IForwarderTokenV2.transfer, (p.refundEOA, amount)));
        emit ReturnedToOwner(account, p.refundEOA, amount, fee);
    }

    // =============================================================== internal

    function _requireValid(B3ForwarderTypesV2.DepositParams calldata p) private view {
        if (p.vault != VAULT) revert WrongVault();
        if (!B3ForwarderTypesV2.recipientV1Valid(p.b3Recipient)) revert BadB3Recipient();
        if (!_refundEOAValid(p.refundEOA)) revert BadRefundEOA();
    }

    function _refundEOAValid(address refundEOA) private view returns (bool) {
        return refundEOA != address(0) && refundEOA != VAULT && refundEOA != TOKEN && refundEOA != address(this);
    }

    /// CREATE2 of the account, isAccount, the one vault approval, Published.
    function _deploy(B3ForwarderTypesV2.DepositParams calldata p, address account) private {
        address deployed = address(new B3Forwarder{salt: B3ForwarderTypesV2.depositSalt(p)}());
        if (deployed != account) revert DeployFailed();
        isAccount[account] = true;
        _tokenCall(account, TOKEN, abi.encodeCall(IForwarderTokenV2.approve, (VAULT, type(uint256).max)));
        emit Published(account, p);
    }

    /// free - fee, after checking fee against the lifetime cap and the free balance.
    function _afterFee(address account, uint96 feeCap, uint256 fee) private view returns (uint256) {
        if (fee > feeCap || feesCharged[account] > feeCap - fee) revert FeeCapExceeded();
        uint256 available = freeBalance(account);
        if (fee > available) revert InsufficientBalance();
        return available - fee;
    }

    function _accrue(address account, uint256 fee, address payee) private {
        if (fee == 0) return;
        feesCharged[account] += fee;
        reserved[account] += fee;
        owed[account][payee] += fee;
    }

    /// A token call made by the account. Accepts an empty or `true` return
    /// (USDT returns nothing); the token's revert data bubbles.
    function _tokenCall(address account, address token, bytes memory data) private {
        bytes memory ret = B3Forwarder(account).exec(token, 0, data);
        if (ret.length != 0 && (ret.length != 32 || abi.decode(ret, (uint256)) != 1)) revert TokenCallFailed();
    }

    function _balanceOf(address account) private view returns (uint256) {
        (bool ok, bytes32 word) = _staticWord(TOKEN, abi.encodeCall(IForwarderTokenV2.balanceOf, (account)));
        if (!ok) revert TokenCallFailed();
        return uint256(word);
    }

    /// One 32-byte answer of a view, or ok = false.
    function _staticWord(address target, bytes memory callData) private view returns (bool ok, bytes32 word) {
        bytes memory data;
        (ok, data) = target.staticcall(callData);
        if (!ok || data.length != 32) return (false, bytes32(0));
        word = abi.decode(data, (bytes32));
    }
}
