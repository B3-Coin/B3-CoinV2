// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

/// The token calls a forwarder account makes. Used only to build calldata:
/// every call goes through a low-level path that accepts an empty or `true`
/// return, because USDT returns nothing.
interface IForwarderTokenV2 {
    function balanceOf(address account) external view returns (uint256);
    function transfer(address to, uint256 amount) external;
    function approve(address spender, uint256 amount) external;
}

/// Parameters, salts and CREATE2 addresses of the bridge V2 forwarders
/// (doc/design/b3-bridge-v2-contracts-spec.md §7.2, normative).
///
/// A forwarder account is the CREATE2 address of B3Forwarder deployed by its
/// factory with salt keccak256(abi.encode(TAG, fields...)), so it can be paid
/// before it exists and B3 wallets derive it offline from the factory and
/// FORWARDER_INIT_CODE_HASH pins:
///
///   depositSalt = keccak256(abi.encode(DEPOSIT_TAG, vault, b3Recipient,
///                                      refundEOA, feeCap, notAfter))
///   exitSalt    = keccak256(abi.encode(EXIT_TAG, dstEid, to,
///                                      maxBridgeFeeBps, feeCap, refundEOA))
///   account     = address(keccak256(0xff ‖ factory ‖ salt ‖ initCodeHash)[12:])
///
/// Each salt preimage is 192 bytes: six 32-byte words, every field left-padded
/// (big-endian) in the order above. The CREATE2 preimage is 85 bytes. B3's
/// src/modern/bridge_forwarder.h and the shared vectors in
/// test/vectors/bridge_v2_vectors.json reproduce all three.
///
/// This is a separate library from B3TypesV2 so the verifier and vault keep
/// their metadata and code hashes. Tag hashes are hard-coded (recomputed with
/// `cast keccak`); the forwarder tests check each one against its string.
library B3ForwarderTypesV2 {
    /// keccak256("B3/FWD/DEPOSIT/V2")
    bytes32 internal constant DEPOSIT_TAG = 0xcc2b88b5788e6a3d2f51c52fcb908e55a5386859799df6e4878912fdb1b01d2a;
    /// keccak256("B3/FWD/EXIT/OFT/V2")
    bytes32 internal constant EXIT_TAG = 0x0fdc29b9af6b10d38ef4781cb890c8a6d762464db73cc2f5800457cdf07b4c87;

    /// Upper bound of ExitParams.maxBridgeFeeBps (0.5%).
    uint16 internal constant MAX_BRIDGE_FEE_BPS = 50;
    /// Basis-point denominator.
    uint256 internal constant BPS_DENOMINATOR = 10_000;

    /// abi.encode(TAG, five fields): six words.
    uint256 internal constant SALT_PREIMAGE_LENGTH = 192;
    /// 0xff ‖ factory ‖ salt ‖ initCodeHash.
    uint256 internal constant CREATE2_PREIMAGE_LENGTH = 85;

    /// Parameters of one deposit forwarder F_d (route (b), spec §7). The
    /// account address commits to every field.
    struct DepositParams {
        address vault; // the Vault V2 the flush deposits into; must equal the factory's VAULT
        bytes32 b3Recipient; // RECIPIENT_V1 word the vault's Deposit event carries
        address refundEOA; // the user's Ethereum EOA: refund at any time, returnToOwner target
        uint96 feeCap; // lifetime bound on fees charged to the account (D5)
        uint64 notAfter; // unix seconds; flush until, returnToOwner after
    }

    /// Parameters of one exit forwarder F_x (route (d3), spec §8). The account
    /// is reusable: every forward empties it and pays at most feeCap.
    struct ExitParams {
        uint32 dstEid; // LayerZero endpoint id of the destination (Arbitrum 30110)
        bytes32 to; // destination address word: upper 12 bytes zero, lower 20 non-zero
        uint16 maxBridgeFeeBps; // OFT slippage bound, at most MAX_BRIDGE_FEE_BPS
        uint96 feeCap; // per-forward bound on the relayer fee
        address refundEOA; // the user's Ethereum EOA: withdrawOnEthereum
    }

    /// RECIPIENT_V1: 11 zero bytes, version byte 1, then a non-zero 20-byte
    /// B3 key hash (doc/design/b3-bridge-recipient-encoding.md), the same
    /// test B3VaultV2.deposit applies.
    function recipientV1Valid(bytes32 recipient) internal pure returns (bool) {
        uint256 word = uint256(recipient);
        return word >> 160 == 1 && (word & type(uint160).max) != 0;
    }

    /// An EVM address word: upper 12 bytes zero, lower 20 bytes non-zero.
    function addressWordValid(bytes32 word) internal pure returns (bool) {
        uint256 value = uint256(word);
        return value >> 160 == 0 && value != 0;
    }

    /// The exit factory's isValid (spec §8) for the pinned destination:
    /// dstEid matches, `to` is an address word, maxBridgeFeeBps <= 50 and
    /// refundEOA is non-zero. EOA-ness of refundEOA cannot be checked on chain;
    /// wallets prove it with an EIP-191 signature.
    function exitParamsValid(ExitParams memory p, uint32 pinnedDstEid) internal pure returns (bool) {
        return pinnedDstEid != 0 && p.dstEid == pinnedDstEid && addressWordValid(p.to)
            && p.maxBridgeFeeBps <= MAX_BRIDGE_FEE_BPS && p.refundEOA != address(0);
    }

    /// The 192-byte deposit salt preimage.
    function depositSaltPreimage(DepositParams memory p) internal pure returns (bytes memory) {
        return abi.encode(DEPOSIT_TAG, p.vault, p.b3Recipient, p.refundEOA, p.feeCap, p.notAfter);
    }

    /// The 192-byte exit salt preimage.
    function exitSaltPreimage(ExitParams memory p) internal pure returns (bytes memory) {
        return abi.encode(EXIT_TAG, p.dstEid, p.to, p.maxBridgeFeeBps, p.feeCap, p.refundEOA);
    }

    function depositSalt(DepositParams memory p) internal pure returns (bytes32) {
        return keccak256(depositSaltPreimage(p));
    }

    function exitSalt(ExitParams memory p) internal pure returns (bytes32) {
        return keccak256(exitSaltPreimage(p));
    }

    /// EIP-1014 address of `initCodeHash` deployed by `factory` with `salt`.
    function create2Address(address factory, bytes32 salt, bytes32 initCodeHash) internal pure returns (address) {
        return address(uint160(uint256(keccak256(abi.encodePacked(bytes1(0xff), factory, salt, initCodeHash)))));
    }

    /// minAmountLD of an OFT send of `amount` with at most `bps` bridge fee.
    function minAmountAfterFee(uint256 amount, uint16 bps) internal pure returns (uint256) {
        return amount - (amount * bps) / BPS_DENOMINATOR;
    }
}
