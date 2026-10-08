// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

// REGTEST ONLY, never imported by src/ or script/.

import {IOFTV2} from "../src/v2/IOFTV2.sol";
import {IMockOApp, MockLzEndpoint, MockLzGuid} from "./MockLzEndpoint.sol";

/// Shared OApp/OFT core of MockOFTAdapter (anvil 31337) and MockUSDT0
/// (anvil 31338), written from scratch after LayerZero's OFTCore. `send`,
/// `quoteSend` and `token` use IOFTV2's structs, so their selectors and ABI
/// encodings are the ones B3OftExitFactory and wallets call on mainnet.
///
/// Message: abi.encodePacked(bytes32 to, uint64 amount), 40 bytes, no
/// compose. Local and shared decimals are both 6, so the amount carries no
/// dust and must fit in uint64 (AmountSDOverflowed).
///
/// send: the peer of dstEid must be set (NoPeer); msg.value must equal
/// fee.nativeFee (NotEnoughNative, LayerZero's exact-fee rule) and cover the
/// endpoint's quote (NotEnoughNative); LZ-token fees, compose messages and OFT
/// commands are refused. amountReceivedLD = amountLD less `feeBps` (owner-set,
/// default 0) must reach minAmountLD (SlippageExceeded). The subclass debits
/// amountLD from msg.sender (adapter: lock; OFT: burn), then the endpoint
/// sends the message to the peer. extraOptions are accepted and ignored.
///
/// lzReceive: only the endpoint, only from the peer of srcEid; the subclass
/// credits `to` (adapter: transfer; OFT: mint). A revert leaves the packet
/// undelivered at the endpoint, so the relayer may deliver it again.
abstract contract MockOFTBase is IMockOApp {
    /// abi.encodePacked(bytes32 to, uint64 amount).
    uint256 public constant MESSAGE_LENGTH = 40;
    uint256 public constant BPS_DENOMINATOR = 10_000;

    MockLzEndpoint public immutable endpoint;
    /// endpoint.eid(), cached.
    uint32 public immutable localEid;

    address public owner;
    /// Peer OApp on each remote endpoint id (address word).
    mapping(uint32 eid => bytes32 peer) public peers;
    /// Bridge fee withheld from every send, in basis points of amountLD.
    uint16 public feeBps;

    event PeerSet(uint32 eid, bytes32 peer);
    event FeeBpsSet(uint16 feeBps);
    /// LayerZero OFTCore's events.
    event OFTSent(
        bytes32 indexed guid, uint32 dstEid, address indexed fromAddress, uint256 amountSentLD, uint256 amountReceivedLD
    );
    event OFTReceived(bytes32 indexed guid, uint32 srcEid, address indexed toAddress, uint256 amountReceivedLD);

    error NotOwner();
    error BadConfig();
    error BadFeeBps();
    error NoPeer(uint32 eid);
    error OnlyEndpoint(address caller);
    error OnlyPeer(uint32 eid, bytes32 sender);
    error NotEnoughNative(uint256 msgValue);
    error LzTokenUnavailable();
    error ComposeUnsupported();
    error SlippageExceeded(uint256 amountReceivedLD, uint256 minAmountLD);
    error AmountSDOverflowed(uint256 amountLD);
    error BadMessage();

    modifier onlyOwner() {
        if (msg.sender != owner) revert NotOwner();
        _;
    }

    constructor(address endpoint_) {
        if (endpoint_.code.length == 0) revert BadConfig();
        endpoint = MockLzEndpoint(endpoint_);
        localEid = MockLzEndpoint(endpoint_).eid();
        owner = msg.sender;
    }

    // ======================================================== OFT interface

    /// The ERC20 this OFT moves.
    function token() public view virtual returns (address);

    /// Whether senders must approve this contract first.
    function approvalRequired() external pure virtual returns (bool);

    /// The fee `send` will charge for `sp`. Reverts as `send` would for a
    /// missing peer, slippage, an oversized amount or compose.
    function quoteSend(IOFTV2.SendParam calldata sp, bool payInLzToken)
        external
        view
        returns (IOFTV2.MessagingFee memory)
    {
        if (payInLzToken) revert LzTokenUnavailable();
        _checkSendParam(sp);
        return IOFTV2.MessagingFee(endpoint.quote(), 0);
    }

    /// Debits sp.amountLD from msg.sender and sends sp.to the received amount
    /// on sp.dstEid. Any endpoint refund goes to `refundAddress`.
    function send(IOFTV2.SendParam calldata sp, IOFTV2.MessagingFee calldata fee, address refundAddress)
        external
        payable
        returns (IOFTV2.MessagingReceipt memory msgReceipt, IOFTV2.OFTReceipt memory oftReceipt)
    {
        if (fee.lzTokenFee != 0) revert LzTokenUnavailable();
        if (msg.value != fee.nativeFee) revert NotEnoughNative(msg.value);
        uint256 required = endpoint.quote();
        if (fee.nativeFee < required) revert NotEnoughNative(msg.value);
        (bytes32 peer, uint256 amountReceivedLD) = _checkSendParam(sp);

        _debit(msg.sender, sp.amountLD);
        // casting to 'uint64' is safe because _checkSendParam bounds amountLD,
        // and amountReceivedLD <= amountLD, by type(uint64).max
        // forge-lint: disable-next-line(unsafe-typecast)
        bytes memory message = abi.encodePacked(sp.to, uint64(amountReceivedLD));
        (bytes32 guid, uint64 nonce) = endpoint.send{value: msg.value}(sp.dstEid, peer, message, refundAddress);
        emit OFTSent(guid, sp.dstEid, msg.sender, sp.amountLD, amountReceivedLD);
        msgReceipt = IOFTV2.MessagingReceipt(guid, nonce, IOFTV2.MessagingFee(required, 0));
        oftReceipt = IOFTV2.OFTReceipt(sp.amountLD, amountReceivedLD);
    }

    // ============================================================ OApp hook

    /// Credits the packet's `to` with its amount. Only the endpoint, only from
    /// the peer of `srcEid`.
    function lzReceive(uint32 srcEid, bytes32 sender, uint64 nonce, bytes calldata message) external {
        if (msg.sender != address(endpoint)) revert OnlyEndpoint(msg.sender);
        bytes32 peer = peers[srcEid];
        if (peer == bytes32(0) || peer != sender) revert OnlyPeer(srcEid, sender);
        if (message.length != MESSAGE_LENGTH) revert BadMessage();
        // LayerZero's bytes32ToAddress: the low 20 bytes.
        address to = address(uint160(uint256(bytes32(message[0:32]))));
        uint256 amount = uint64(bytes8(message[32:40]));
        to = _credit(to, amount);
        bytes32 guid = MockLzGuid.generate(nonce, srcEid, sender, localEid, bytes32(uint256(uint160(address(this)))));
        emit OFTReceived(guid, srcEid, to, amount);
    }

    // ================================================================== owner

    function setPeer(uint32 eid, bytes32 peer) external onlyOwner {
        peers[eid] = peer;
        emit PeerSet(eid, peer);
    }

    function setFeeBps(uint16 bps) external onlyOwner {
        if (bps > BPS_DENOMINATOR) revert BadFeeBps();
        feeBps = bps;
        emit FeeBpsSet(bps);
    }

    // =============================================================== internal

    /// The peer of sp.dstEid and the amount sp.to receives.
    function _checkSendParam(IOFTV2.SendParam calldata sp)
        private
        view
        returns (bytes32 peer, uint256 amountReceivedLD)
    {
        if (sp.composeMsg.length != 0 || sp.oftCmd.length != 0) {
            revert ComposeUnsupported();
        }
        peer = peers[sp.dstEid];
        if (peer == bytes32(0)) revert NoPeer(sp.dstEid);
        if (sp.amountLD > type(uint64).max) revert AmountSDOverflowed(sp.amountLD);
        amountReceivedLD = sp.amountLD - sp.amountLD * feeBps / BPS_DENOMINATOR;
        if (amountReceivedLD < sp.minAmountLD) revert SlippageExceeded(amountReceivedLD, sp.minAmountLD);
    }

    /// Takes `amount` from `from` (lock or burn).
    function _debit(address from, uint256 amount) internal virtual;

    /// Gives `amount` to `to` (unlock or mint); returns the address credited.
    function _credit(address to, uint256 amount) internal virtual returns (address credited);
}
