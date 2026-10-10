// SPDX-License-Identifier: MIT
pragma solidity ^0.8.28;

import {B3Forwarder} from "./B3Forwarder.sol";
import {B3ForwarderTypesV2, IForwarderTokenV2} from "./B3ForwarderTypesV2.sol";
import {IOFTV2} from "./IOFTV2.sol";

/// Factory of exit forwarders F_x (doc/design/b3-bridge-v2-contracts-spec.md §8;
/// route (d3)): Vault V2 release -> F_x -> USDT0 OFT adapter -> Arbitrum.
///
/// A B3 burn names F_x = accountOf(p) as its Ethereum recipient and carries p
/// in its exit block, so B3 checks the address and every relayer learns p
/// from the burn record. After the vault's release pays the account, anyone
/// may call forward, normally in the same B3RelayBatch transaction: it pays
/// the caller's fee (at most p.feeCap per forward) to `feeRecipient` and sends
/// the whole rest through the pinned OFT adapter to p.to on DST_EID, with at
/// most p.maxBridgeFeeBps of bridge slippage. Each forward empties the
/// account, so a reused F_x address pays at most feeCap per withdrawal.
///
/// Every token, adapter, endpoint id and executor option is fixed at
/// construction. An adapter, OFT or endpoint change needs a new factory and a
/// B3 `exit_forwarder` re-pin. No owner, admin or pause.
///
/// Escapes: a LayerZero or USDT0 pause, or a short native fee, makes forward
/// revert and the funds wait; p.refundEOA can always take them on Ethereum
/// with withdrawOnEthereum.
///
/// No NUMBER opcode. The reentrancy lock is transient; the only storage is
/// the write-once executor options.
contract B3OftExitFactory {
    /// USDT on Ethereum, the adapter's token.
    address public immutable TOKEN;
    /// The USDT0 OFT adapter (Ethereum) that locks USDT and messages Arbitrum.
    address public immutable ADAPTER;
    /// LayerZero endpoint id of the destination (Arbitrum One: 30110).
    uint32 public immutable DST_EID;
    /// keccak256(type(B3Forwarder).creationCode); the manifest pin B3 derives with.
    bytes32 public immutable FORWARDER_INIT_CODE_HASH;

    /// Executor options passed with every send; written once by the constructor.
    bytes public extraOptions;

    bool private transient _entered;

    /// p is public (optional; relayers also read it from the B3 burn record).
    event Published(address indexed account, B3ForwarderTypesV2.ExitParams p);
    event Forwarded(
        address indexed account,
        bytes32 indexed to,
        bytes32 indexed guid,
        uint256 amountSent,
        uint256 amountReceived,
        uint256 fee,
        address feeRecipient,
        uint256 nativeFee
    );
    /// token == address(0) is native ETH.
    event WithdrawnOnEthereum(address indexed account, address indexed token, address indexed to, uint256 amount);

    error BadConfig();
    error Reentrancy();
    error InvalidParams();
    error FeeCapExceeded();
    error BadFeeRecipient();
    error ZeroNativeRefundTo();
    error NothingToForward();
    error NativeFeeShort(uint256 required, uint256 provided);
    error NativeRefundFailed();
    error DeployFailed();
    error NotRefundEOA();
    error BadWithdrawTarget();
    error ZeroAmount();
    error InsufficientBalance();
    error NoCode(address target);
    error TokenCallFailed();

    modifier nonReentrant() {
        if (_entered) revert Reentrancy();
        _entered = true;
        _;
        _entered = false;
    }

    /// `adapter` must report `token` as its token(); `dstEid` is non-zero.
    constructor(address token, address adapter, uint32 dstEid, bytes memory extraOptions_) {
        if (token.code.length == 0 || adapter.code.length == 0 || dstEid == 0) revert BadConfig();
        (bool ok, bytes memory data) = adapter.staticcall(abi.encodeCall(IOFTV2.token, ()));
        if (!ok || data.length != 32 || abi.decode(data, (bytes32)) != bytes32(uint256(uint160(token)))) {
            revert BadConfig();
        }
        TOKEN = token;
        ADAPTER = adapter;
        DST_EID = dstEid;
        FORWARDER_INIT_CODE_HASH = keccak256(type(B3Forwarder).creationCode);
        extraOptions = extraOptions_;
    }

    // ================================================================== views

    /// The F_x address of `p`, deployed or not. Defined for every p.
    function accountOf(B3ForwarderTypesV2.ExitParams calldata p) public view returns (address) {
        bytes32 salt = B3ForwarderTypesV2.exitSalt(p);
        return B3ForwarderTypesV2.create2Address(address(this), salt, FORWARDER_INIT_CODE_HASH);
    }

    /// dstEid == DST_EID; `to` an address word; maxBridgeFeeBps <= 50;
    /// refundEOA non-zero. EOA-ness of refundEOA is the wallet's check.
    function isValid(B3ForwarderTypesV2.ExitParams calldata p) public view returns (bool) {
        return B3ForwarderTypesV2.exitParamsValid(p, DST_EID);
    }

    /// The native fee a forward of `amount` for `p` must carry.
    function quoteForward(B3ForwarderTypesV2.ExitParams calldata p, uint256 amount)
        external
        view
        returns (uint256 nativeFee)
    {
        return IOFTV2(ADAPTER).quoteSend(_sendParam(p, amount), false).nativeFee;
    }

    // ============================================================== lifecycle

    /// Publishes a valid `p` (anyone) without deploying it; no state.
    function publish(B3ForwarderTypesV2.ExitParams calldata p) external returns (address account) {
        if (!isValid(p)) revert InvalidParams();
        account = accountOf(p);
        emit Published(account, p);
    }

    /// Sends the account's whole token balance less `fee` to p.to on DST_EID
    /// (anyone), paying `fee` (at most p.feeCap) to `feeRecipient`. msg.value
    /// must cover the adapter's quote; the rest goes back to `nativeRefundTo`.
    /// Deploys the account on demand. Returns the OFT's amountSentLD.
    function forward(
        B3ForwarderTypesV2.ExitParams calldata p,
        uint256 fee,
        address feeRecipient,
        address nativeRefundTo
    ) external payable nonReentrant returns (uint256 amountSent) {
        if (!isValid(p)) revert InvalidParams();
        if (fee > p.feeCap) revert FeeCapExceeded();
        address account = accountOf(p);
        if (fee != 0 && (feeRecipient == address(0) || feeRecipient == account || feeRecipient == address(this))) {
            revert BadFeeRecipient();
        }
        if (nativeRefundTo == address(0)) revert ZeroNativeRefundTo();
        if (account.code.length == 0) _deploy(p, account);

        uint256 balance = _balanceOf(account);
        if (balance <= fee) revert NothingToForward();
        uint256 amount = balance - fee;
        IOFTV2.SendParam memory sp = _sendParam(p, amount);
        uint256 nativeFee = IOFTV2(ADAPTER).quoteSend(sp, false).nativeFee;
        if (msg.value < nativeFee) revert NativeFeeShort(nativeFee, msg.value);

        if (fee != 0) _tokenCall(account, TOKEN, abi.encodeCall(IForwarderTokenV2.transfer, (feeRecipient, fee)));
        // USDT refuses a non-zero to non-zero allowance change: go through zero.
        _tokenCall(account, TOKEN, abi.encodeCall(IForwarderTokenV2.approve, (ADAPTER, 0)));
        _tokenCall(account, TOKEN, abi.encodeCall(IForwarderTokenV2.approve, (ADAPTER, amount)));
        bytes memory ret = B3Forwarder(account).exec{value: nativeFee}(
            ADAPTER, nativeFee, abi.encodeCall(IOFTV2.send, (sp, IOFTV2.MessagingFee(nativeFee, 0), nativeRefundTo))
        );
        _tokenCall(account, TOKEN, abi.encodeCall(IForwarderTokenV2.approve, (ADAPTER, 0)));
        (IOFTV2.MessagingReceipt memory receipt, IOFTV2.OFTReceipt memory oftReceipt) =
            abi.decode(ret, (IOFTV2.MessagingReceipt, IOFTV2.OFTReceipt));
        amountSent = oftReceipt.amountSentLD;

        uint256 change = msg.value - nativeFee;
        if (change != 0) {
            (bool ok,) = nativeRefundTo.call{value: change}("");
            if (!ok) revert NativeRefundFailed();
        }
        emit Forwarded(
            account, p.to, receipt.guid, amountSent, oftReceipt.amountReceivedLD, fee, feeRecipient, nativeFee
        );
    }

    /// Sends `amount` of `token` (address(0): native ETH) from the account of
    /// `p` to `to` on Ethereum. Only p.refundEOA; any p, valid or not, at any
    /// time; deploys the account on demand.
    function withdrawOnEthereum(B3ForwarderTypesV2.ExitParams calldata p, address token, address to, uint256 amount)
        external
        nonReentrant
    {
        if (msg.sender != p.refundEOA) revert NotRefundEOA();
        address account = accountOf(p);
        if (to == address(0) || to == account || to == address(this)) revert BadWithdrawTarget();
        if (amount == 0) revert ZeroAmount();
        if (account.code.length == 0) _deploy(p, account);

        if (token == address(0)) {
            if (amount > account.balance) revert InsufficientBalance();
            B3Forwarder(account).exec(to, amount, "");
        } else {
            if (token.code.length == 0) revert NoCode(token);
            _tokenCall(account, token, abi.encodeCall(IForwarderTokenV2.transfer, (to, amount)));
        }
        emit WithdrawnOnEthereum(account, token, to, amount);
    }

    // =============================================================== internal

    function _deploy(B3ForwarderTypesV2.ExitParams calldata p, address account) private {
        address deployed = address(new B3Forwarder{salt: B3ForwarderTypesV2.exitSalt(p)}());
        if (deployed != account) revert DeployFailed();
    }

    /// SendParam(DST_EID, p.to, amount, amount less maxBridgeFeeBps, extraOptions, "", "").
    function _sendParam(B3ForwarderTypesV2.ExitParams calldata p, uint256 amount)
        private
        view
        returns (IOFTV2.SendParam memory)
    {
        return IOFTV2.SendParam({
            dstEid: DST_EID,
            to: p.to,
            amountLD: amount,
            minAmountLD: B3ForwarderTypesV2.minAmountAfterFee(amount, p.maxBridgeFeeBps),
            extraOptions: extraOptions,
            composeMsg: "",
            oftCmd: ""
        });
    }

    /// A token call made by the account. Accepts an empty or `true` return
    /// (USDT returns nothing); the token's revert data bubbles.
    function _tokenCall(address account, address token, bytes memory data) private {
        bytes memory ret = B3Forwarder(account).exec(token, 0, data);
        if (ret.length != 0 && (ret.length != 32 || abi.decode(ret, (uint256)) != 1)) revert TokenCallFailed();
    }

    function _balanceOf(address account) private view returns (uint256) {
        (bool ok, bytes memory data) = TOKEN.staticcall(abi.encodeCall(IForwarderTokenV2.balanceOf, (account)));
        if (!ok || data.length != 32) revert TokenCallFailed();
        return abi.decode(data, (uint256));
    }
}
