// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

// REGTEST ONLY, never imported by src/ or script/.

import {MockOFTBase} from "./MockOFTBase.sol";

/// USDT0 OFT adapter test double on the Ethereum anvil (31337, endpoint id
/// 30101), written from scratch. It stands in for the adapter
/// 0x6C96dE32CEa08842dcc4058c14d3aaAD7Fa41dee that B3OftExitFactory pins:
/// `token()` is MockTetherToken, `send` locks USDT from msg.sender and
/// messages MockUSDT0 on 31338 (route (d3)), and a delivery from 31338 pays
/// the packet's `to` in USDT by plain transfer, with no compose (route (b):
/// an undeployed F_d is credited like any address).
///
/// The harness seeds the adapter with locked USDT, as the real adapter holds
/// the USDT behind every USDT0 in circulation. A delivery the adapter cannot
/// pay (not seeded, Tether paused or the adapter blacklisted) reverts and
/// stays retryable at the endpoint.
///
/// Token calls accept an empty or `true` return (Tether returns nothing).
/// The lock is measured by balance delta and must equal amountLD (LockShort),
/// so a Tether fee switch shows up as a failed send rather than an
/// under-backed message. The owner sets peers and the bridge fee
/// (MockOFTBase); the withheld fee stays locked here.
contract MockOFTAdapter is MockOFTBase {
    /// MockTetherToken on 31337.
    address public immutable innerToken;

    error LockShort(uint256 expected, uint256 locked);
    error TokenCallFailed();

    constructor(address token_, address endpoint_) MockOFTBase(endpoint_) {
        if (token_.code.length == 0) revert BadConfig();
        innerToken = token_;
    }

    function token() public view override returns (address) {
        return innerToken;
    }

    function approvalRequired() external pure override returns (bool) {
        return true;
    }

    /// USDT held by the adapter: the locked backing plus withheld fees.
    function lockedBalance() external view returns (uint256) {
        return _balanceOfThis();
    }

    function _debit(address from, uint256 amount) internal override {
        uint256 before = _balanceOfThis();
        _tokenCall(abi.encodeWithSignature("transferFrom(address,address,uint256)", from, address(this), amount));
        uint256 afterLock = _balanceOfThis();
        uint256 locked = afterLock > before ? afterLock - before : 0;
        if (locked != amount) revert LockShort(amount, locked);
    }

    function _credit(address to, uint256 amount) internal override returns (address) {
        _tokenCall(abi.encodeWithSignature("transfer(address,uint256)", to, amount));
        return to;
    }

    function _balanceOfThis() private view returns (uint256) {
        (bool ok, bytes memory data) =
            innerToken.staticcall(abi.encodeWithSignature("balanceOf(address)", address(this)));
        if (!ok || data.length != 32) revert TokenCallFailed();
        return abi.decode(data, (uint256));
    }

    /// The token's revert data bubbles; an empty or `true` return is success.
    function _tokenCall(bytes memory data) private {
        (bool ok, bytes memory ret) = innerToken.call(data);
        if (!ok) {
            assembly ("memory-safe") {
                revert(add(ret, 32), mload(ret))
            }
        }
        if (ret.length != 0 && (ret.length != 32 || abi.decode(ret, (uint256)) != 1)) revert TokenCallFailed();
    }
}
