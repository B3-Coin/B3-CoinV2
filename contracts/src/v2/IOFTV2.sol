// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

/// The subset of LayerZero's IOFT that the bridge V2 exit forwarder uses
/// (https://github.com/LayerZero-Labs/devtools/blob/main/packages/oft-evm/contracts/interfaces/IOFT.sol).
///
/// Struct layouts and function signatures are LayerZero's, so selectors and
/// ABI encodings match the USDT0 OFT adapter on Ethereum
/// (0x6C96dE32CEa08842dcc4058c14d3aaAD7Fa41dee) and the USDT0 OFT on Arbitrum.
/// Written from the published interface; no LayerZero code is vendored.
interface IOFTV2 {
    struct SendParam {
        uint32 dstEid; // destination endpoint id
        bytes32 to; // recipient address word on the destination chain
        uint256 amountLD; // amount in local decimals
        uint256 minAmountLD; // minimum received in local decimals (slippage bound)
        bytes extraOptions; // executor options added to the OApp's enforced options
        bytes composeMsg; // empty: no compose
        bytes oftCmd; // empty: no OFT command
    }

    struct MessagingFee {
        uint256 nativeFee;
        uint256 lzTokenFee;
    }

    struct MessagingReceipt {
        bytes32 guid;
        uint64 nonce;
        MessagingFee fee;
    }

    struct OFTReceipt {
        uint256 amountSentLD; // debited from the sender
        uint256 amountReceivedLD; // credited on the destination
    }

    /// The ERC20 the OFT moves (the adapter's locked token, or the OFT itself).
    function token() external view returns (address);

    /// The fee `send` will charge for `sendParam`.
    function quoteSend(SendParam calldata sendParam, bool payInLzToken) external view returns (MessagingFee memory);

    /// Debits `sendParam.amountLD` from msg.sender (an adapter pulls it with
    /// transferFrom) and sends it to `sendParam.to` on `sendParam.dstEid`.
    /// msg.value must equal `fee.nativeFee`; any refund goes to `refundAddress`.
    function send(SendParam calldata sendParam, MessagingFee calldata fee, address refundAddress)
        external
        payable
        returns (MessagingReceipt memory, OFTReceipt memory);
}
