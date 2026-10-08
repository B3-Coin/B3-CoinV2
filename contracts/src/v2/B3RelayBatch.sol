// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {B3Types} from "../IB3FinalityProver.sol";
import {B3TypesV2} from "./B3TypesV2.sol";
import {B3ForwarderTypesV2} from "./B3ForwarderTypesV2.sol";
import {IB3FinalityVerifierV2} from "./IB3FinalityVerifierV2.sol";

/// The release surface of B3VaultV2 that the batch calls.
interface IB3VaultV2Release {
    function released(uint64 withdrawalId) external view returns (bool);

    function release(B3TypesV2.WithdrawalV2 calldata w, bytes32[32] calldata path) external;
}

/// The surface of B3OftExitFactory (F_x, route (d3)) that the batch calls.
/// forward's return value (amountSent) is left undeclared, so odd return data
/// from a wrong target cannot revert the batch while being decoded.
interface IB3OftExitForward {
    function accountOf(B3ForwarderTypesV2.ExitParams calldata p) external view returns (address);

    function forward(
        B3ForwarderTypesV2.ExitParams calldata p,
        uint256 fee,
        address feeRecipient,
        address nativeRefundTo
    ) external payable;
}

/// Stateless batch relayer of bridge V2 (doc/design/b3-bridge-v2-contracts-spec.md §9).
///
/// One transaction relays certificates to a B3FinalityVerifierV2, then
/// releases leaves V2 from a B3VaultV2 and, with relayWithForwards, then
/// forwards exit forwarders F_x through their B3OftExitFactory (route (d3):
/// the release pays F_x, the forward sends it on to Arbitrum, and the F_x fee
/// pays for both). Every item is tried on its own: a failing certificate,
/// release or forward emits an event and the batch moves on. Each try
/// forwards at most the caller's per-item gas cap (`certGas`, `releaseGas`,
/// `forwardGas`), because a malformed BLS witness makes an EIP-2537
/// precompile burn all gas forwarded to it: without a cap one such item
/// would leave the rest of the batch about 1/64 of its gas. With a cap a bad
/// item costs at most its cap. Size the caps from the gas table in
/// contracts/README.md (a 64-member certificate executes in about 5.5M gas, a
/// release in about 0.1M, a first forward in about 0.4M); an item that needs
/// more than its cap fails out of gas and is reported like any other failure.
/// type(uint256).max forwards all available gas (63/64, EIP-150).
/// Certificates run first and in order, so a release may use a root
/// certified earlier in the same batch; with root history (spec §6.4) it may
/// equally use any root accepted before. Releases run before forwards, so a
/// forward may send what a release in the same batch paid its F_x.
///
/// relay is not payable. relayWithForwards is: msg.value is the native-fee
/// budget of the forwards. Each forward carries its item's `nativeFee` while
/// that fits in what is left of the budget, and is skipped otherwise. A
/// failed forward's ETH comes back to the batch with the revert. The factory
/// returns any part of `nativeFee` above the OFT quote straight to
/// `nativeRefundTo`, and the batch returns the unspent budget there at the
/// end. The budget is counted per call, not read from the balance, so a
/// re-entrant call can never spend or return another call's ETH.
///
/// The batch has no constructor, no storage, no owner, no receive and no
/// fallback, and grants nothing: the verifier, the vault and the exit factory
/// are permissionless, so the batch can do nothing a direct call could not.
/// The vault pays each leaf to its recipient, and the exit factory pays the
/// fee to the item's `feeRecipient`, never to the batch. The verifier, vault
/// and exit factory are arguments, chosen by the caller; a wrong target fails
/// only the caller's own items. The batch is not a B3 pin.
contract B3RelayBatch {
    /// The arguments of one IB3FinalityVerifierV2.submitCertificate call.
    struct CertInput {
        B3TypesV2.FinalizedBlockV2 fb;
        B3Types.SetHeader signingSet;
        B3Types.SetHeader successor;
        bytes32[32] ancestryPath;
        bytes proof;
    }

    /// The arguments of one IB3VaultV2Release.release call.
    struct ReleaseItem {
        B3TypesV2.WithdrawalV2 w;
        bytes32[32] path;
    }

    /// The arguments of one IB3OftExitForward.forward call (spec §8-§9) and
    /// the ETH it carries; the batch passes its own `nativeRefundTo`.
    struct ExitForward {
        B3ForwarderTypesV2.ExitParams p;
        uint256 fee; // relayer fee in USDT, at most p.feeCap (the factory checks)
        address feeRecipient;
        uint256 nativeFee; // wei sent with the call: at least the OFT quote
    }

    /// certs[index] reverted with `reason` (the verifier's revert data).
    event CertificateFailed(uint256 indexed index, bytes reason);
    /// releases[index] was not attempted: the vault reports it already released.
    event ReleaseSkipped(uint256 indexed index, uint64 indexed withdrawalId);
    /// releases[index] reverted with `reason` (the vault's revert data).
    event ReleaseFailed(uint256 indexed index, uint64 indexed withdrawalId, bytes reason);
    /// forwards[index] was not attempted: its nativeFee exceeds what is left
    /// of msg.value. `account` is the factory's accountOf(p) (zero if that
    /// view fails).
    event ForwardSkipped(uint256 indexed index, address indexed account);
    /// forwards[index] reverted with `reason` (the factory's revert data); its
    /// ETH stayed with the batch. `account` as for ForwardSkipped.
    event ForwardFailed(uint256 indexed index, address indexed account, bytes reason);

    /// A call to an address without code cannot be caught by try/catch, so a
    /// codeless target named for a non-empty list reverts the whole batch.
    error NoCode(address target);
    /// msg.value is non-zero and nativeRefundTo is the zero address.
    error ZeroNativeRefundTo();
    /// Returning the unspent budget to nativeRefundTo failed.
    error RefundFailed();

    /// Tries each certificate in order with at most `certGas`, then each
    /// release not yet released with at most `releaseGas`. `vault` is unused
    /// when `releases` is empty and `verifier` when `certs` is empty. Returns
    /// how many certificates were accepted and how many releases paid.
    function relay(
        IB3FinalityVerifierV2 verifier,
        CertInput[] calldata certs,
        uint256 certGas,
        address vault,
        ReleaseItem[] calldata releases,
        uint256 releaseGas
    ) external returns (uint256 certsAccepted, uint256 releasesPaid) {
        _requireCode(address(verifier), certs.length);
        _requireCode(vault, releases.length);
        certsAccepted = _relayCertificates(verifier, certs, certGas);
        releasesPaid = _relayReleases(vault, releases, releaseGas);
    }

    /// As relay, then tries each forward in order with at most `forwardGas`
    /// through `exitFactory`, carrying its `nativeFee` out of msg.value; a
    /// forward whose nativeFee exceeds the rest of msg.value is skipped.
    /// Every forward names `nativeRefundTo` for the factory's change, and the
    /// unspent msg.value goes there at the end (RefundFailed reverts the whole
    /// batch). `exitFactory` is unused when `forwards` is empty, and
    /// `nativeRefundTo` may be zero only when msg.value is zero (a forward then
    /// fails in the factory). Returns how many certificates were accepted, how
    /// many releases paid and how many forwards sent.
    function relayWithForwards(
        IB3FinalityVerifierV2 verifier,
        CertInput[] calldata certs,
        uint256 certGas,
        address vault,
        ReleaseItem[] calldata releases,
        uint256 releaseGas,
        address exitFactory,
        ExitForward[] calldata forwards,
        uint256 forwardGas,
        address nativeRefundTo
    ) external payable returns (uint256 certsAccepted, uint256 releasesPaid, uint256 forwardsSent) {
        _requireCode(address(verifier), certs.length);
        _requireCode(vault, releases.length);
        _requireCode(exitFactory, forwards.length);
        if (msg.value != 0 && nativeRefundTo == address(0)) revert ZeroNativeRefundTo();

        certsAccepted = _relayCertificates(verifier, certs, certGas);
        releasesPaid = _relayReleases(vault, releases, releaseGas);
        uint256 unspent;
        (forwardsSent, unspent) = _relayForwards(exitFactory, forwards, forwardGas, nativeRefundTo, msg.value);

        if (unspent != 0) {
            (bool ok,) = nativeRefundTo.call{value: unspent}("");
            if (!ok) revert RefundFailed();
        }
    }

    function _requireCode(address target, uint256 count) private view {
        if (count != 0 && target.code.length == 0) revert NoCode(target);
    }

    function _relayCertificates(IB3FinalityVerifierV2 verifier, CertInput[] calldata certs, uint256 certGas)
        private
        returns (uint256 certsAccepted)
    {
        uint256 certCount = certs.length;
        for (uint256 i = 0; i < certCount; ++i) {
            CertInput calldata c = certs[i];
            try verifier.submitCertificate{gas: certGas}(c.fb, c.signingSet, c.successor, c.ancestryPath, c.proof) {
                ++certsAccepted;
            } catch (bytes memory reason) {
                emit CertificateFailed(i, reason);
            }
        }
    }

    function _relayReleases(address vault, ReleaseItem[] calldata releases, uint256 releaseGas)
        private
        returns (uint256 releasesPaid)
    {
        uint256 releaseCount = releases.length;
        IB3VaultV2Release target = IB3VaultV2Release(vault);
        for (uint256 i = 0; i < releaseCount; ++i) {
            ReleaseItem calldata r = releases[i];
            uint64 withdrawalId = r.w.withdrawalId;
            if (_isReleased(vault, withdrawalId)) {
                emit ReleaseSkipped(i, withdrawalId);
                continue;
            }
            try target.release{gas: releaseGas}(r.w, r.path) {
                ++releasesPaid;
            } catch (bytes memory reason) {
                emit ReleaseFailed(i, withdrawalId, reason);
            }
        }
    }

    /// Tries each forward within `budget` wei; returns how many were sent and
    /// what is left of `budget`. A successful forward spends its whole
    /// nativeFee: the factory returns the change to nativeRefundTo itself.
    function _relayForwards(
        address exitFactory,
        ExitForward[] calldata forwards,
        uint256 forwardGas,
        address nativeRefundTo,
        uint256 budget
    ) private returns (uint256 forwardsSent, uint256 unspent) {
        unspent = budget;
        uint256 forwardCount = forwards.length;
        IB3OftExitForward target = IB3OftExitForward(exitFactory);
        for (uint256 i = 0; i < forwardCount; ++i) {
            ExitForward calldata f = forwards[i];
            uint256 nativeFee = f.nativeFee;
            if (nativeFee > unspent) {
                emit ForwardSkipped(i, _accountOf(exitFactory, f.p, forwardGas));
                continue;
            }
            try target.forward{value: nativeFee, gas: forwardGas}(f.p, f.fee, f.feeRecipient, nativeRefundTo) {
                ++forwardsSent;
                unspent -= nativeFee;
            } catch (bytes memory reason) {
                emit ForwardFailed(i, _accountOf(exitFactory, f.p, forwardGas), reason);
            }
        }
    }

    /// vault.released(id) == true, read by a low-level call so a vault whose
    /// view reverts or answers oddly cannot revert the batch; such an item is
    /// then tried, and its failure reported, like any other.
    function _isReleased(address vault, uint64 withdrawalId) private view returns (bool yes) {
        bytes4 selector = IB3VaultV2Release.released.selector;
        assembly ("memory-safe") {
            mstore(0x00, selector)
            mstore(0x04, withdrawalId)
            let ok := staticcall(gas(), vault, 0x00, 0x24, 0x00, 0x20)
            yes := and(ok, and(eq(returndatasize(), 0x20), eq(mload(0x00), 1)))
        }
    }

    /// exitFactory.accountOf(p) for an event, read by a low-level call with at
    /// most `gasCap`: a factory whose view reverts, burns gas or answers
    /// anything but one address word yields address(0) and cannot revert the
    /// batch.
    function _accountOf(address exitFactory, B3ForwarderTypesV2.ExitParams calldata p, uint256 gasCap)
        private
        view
        returns (address account)
    {
        bytes memory data = abi.encodeCall(IB3OftExitForward.accountOf, (p));
        assembly ("memory-safe") {
            let ok := staticcall(gasCap, exitFactory, add(data, 0x20), mload(data), 0x00, 0x20)
            let word := mload(0x00)
            if and(ok, and(eq(returndatasize(), 0x20), iszero(shr(160, word)))) { account := word }
        }
    }
}
