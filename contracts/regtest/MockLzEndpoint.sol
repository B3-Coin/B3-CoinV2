// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

// REGTEST ONLY, never imported by src/ or script/.

/// The receive hook MockLzEndpoint.deliver calls on a mock OApp.
interface IMockOApp {
    function lzReceive(uint32 srcEid, bytes32 sender, uint64 nonce, bytes calldata message) external;
}

/// LayerZero GUID as EndpointV2 derives it:
/// keccak256(abi.encodePacked(nonce, srcEid, sender, dstEid, receiver)).
library MockLzGuid {
    function generate(uint64 nonce, uint32 srcEid, bytes32 sender, uint32 dstEid, bytes32 receiver)
        internal
        pure
        returns (bytes32)
    {
        return keccak256(abi.encodePacked(nonce, srcEid, sender, dstEid, receiver));
    }
}

/// LayerZero endpoint test double for local anvil chains (regtest plan §3.1,
/// §4.1; route (b) and (d3)), written from scratch. One instance runs on each
/// chain under that chain's mainnet endpoint id (Ethereum 30101 on anvil
/// 31337, Arbitrum 30110 on anvil 31338), so F_x's DST_EID check is the
/// production one.
///
/// Source side: an OApp calls `send`, paying at least `nativeFee` (the rest
/// goes back to `refundAddress`); the endpoint numbers the packet with one
/// outbound counter starting at 1 and emits PacketSent. Destination side: the
/// harness LayerZero relayer copies each PacketSent to the endpoint whose eid
/// is its dstEid and calls `deliver`, which runs the receiver's lzReceive
/// exactly once. A reverting lzReceive reverts the whole delivery, so the
/// packet stays undelivered and can be delivered again later, as with
/// EndpointV2.lzReceive.
///
/// Not modelled: DVNs and their wait (the relayer chooses when to deliver),
/// executor options, compose, LZ-token fees and per-path nonce ordering.
/// Anyone may call `deliver`, and the endpoint cannot see the source chain,
/// so it trusts the relayer for packet contents; it verifies nothing. The
/// owner sets the fee and pauses both directions.
contract MockLzEndpoint {
    /// Default native fee per send: 1e14 wei (0.0001 ETH).
    uint256 public constant DEFAULT_NATIVE_FEE = 1e14;

    /// This chain's LayerZero endpoint id.
    uint32 public immutable eid;

    address public owner;
    /// Native fee every send must carry (wei).
    uint256 public nativeFee = DEFAULT_NATIVE_FEE;
    /// While set, send and deliver revert.
    bool public paused;
    /// Nonce of the last packet sent; the next one is outboundNonce + 1.
    uint64 public outboundNonce;
    /// packetKey(srcEid, sender, nonce) => delivered.
    mapping(bytes32 key => bool) public delivered;

    event PacketSent(
        uint64 indexed nonce, uint32 indexed dstEid, address indexed sender, bytes32 receiver, bytes message
    );
    event PacketDelivered(
        bytes32 indexed guid, uint32 indexed srcEid, bytes32 indexed sender, uint64 nonce, address receiver
    );
    event NativeFeeSet(uint256 nativeFee);
    event PausedSet(bool paused);

    error NotOwner();
    error BadEid();
    error EndpointPaused();
    error InsufficientFee(uint256 required, uint256 supplied);
    error ZeroRefundAddress();
    error RefundFailed();
    error AlreadyDelivered(uint32 srcEid, bytes32 sender, uint64 nonce);
    error NoCode(address receiver);

    modifier onlyOwner() {
        if (msg.sender != owner) revert NotOwner();
        _;
    }

    constructor(uint32 eid_) {
        if (eid_ == 0) revert BadEid();
        eid = eid_;
        owner = msg.sender;
    }

    // ================================================================== views

    /// The native fee one send costs.
    function quote() external view returns (uint256) {
        return nativeFee;
    }

    /// Whether the packet `nonce` from `sender` on `srcEid` was delivered here.
    function isDelivered(uint32 srcEid, bytes32 sender, uint64 nonce) external view returns (bool) {
        return delivered[packetKey(srcEid, sender, nonce)];
    }

    function packetKey(uint32 srcEid, bytes32 sender, uint64 nonce) public pure returns (bytes32) {
        return keccak256(abi.encode(srcEid, sender, nonce));
    }

    function guidOf(uint64 nonce, uint32 srcEid, bytes32 sender, uint32 dstEid, bytes32 receiver)
        external
        pure
        returns (bytes32)
    {
        return MockLzGuid.generate(nonce, srcEid, sender, dstEid, receiver);
    }

    // ================================================================= source

    /// Sends `message` from the calling OApp to `receiver` on `dstEid`.
    /// msg.value must cover nativeFee; the excess goes to `refundAddress`.
    function send(uint32 dstEid, bytes32 receiver, bytes calldata message, address refundAddress)
        external
        payable
        returns (bytes32 guid, uint64 nonce)
    {
        if (paused) revert EndpointPaused();
        if (dstEid == 0) revert BadEid();
        uint256 fee = nativeFee;
        if (msg.value < fee) revert InsufficientFee(fee, msg.value);
        nonce = ++outboundNonce;
        guid = MockLzGuid.generate(nonce, eid, bytes32(uint256(uint160(msg.sender))), dstEid, receiver);
        emit PacketSent(nonce, dstEid, msg.sender, receiver, message);

        uint256 excess = msg.value - fee;
        if (excess != 0) {
            if (refundAddress == address(0)) revert ZeroRefundAddress();
            (bool ok,) = refundAddress.call{value: excess}("");
            if (!ok) revert RefundFailed();
        }
    }

    // ============================================================ destination

    /// Delivers packet `nonce` from `sender` on `srcEid` to `receiver` (anyone;
    /// the harness relayer). The receiver's revert data bubbles and leaves the
    /// packet undelivered.
    function deliver(uint32 srcEid, bytes32 sender, uint64 nonce, address receiver, bytes calldata message) external {
        if (paused) revert EndpointPaused();
        bytes32 key = packetKey(srcEid, sender, nonce);
        if (delivered[key]) revert AlreadyDelivered(srcEid, sender, nonce);
        if (receiver.code.length == 0) revert NoCode(receiver);
        // Set before the call, so a re-entrant delivery of the same packet fails.
        delivered[key] = true;
        (bool ok, bytes memory reason) =
            receiver.call(abi.encodeCall(IMockOApp.lzReceive, (srcEid, sender, nonce, message)));
        if (!ok) {
            assembly ("memory-safe") {
                revert(add(reason, 32), mload(reason))
            }
        }
        emit PacketDelivered(
            MockLzGuid.generate(nonce, srcEid, sender, eid, bytes32(uint256(uint160(receiver)))),
            srcEid,
            sender,
            nonce,
            receiver
        );
    }

    // ================================================================== owner

    function setNativeFee(uint256 fee) external onlyOwner {
        nativeFee = fee;
        emit NativeFeeSet(fee);
    }

    function setPaused(bool paused_) external onlyOwner {
        paused = paused_;
        emit PausedSet(paused_);
    }
}
