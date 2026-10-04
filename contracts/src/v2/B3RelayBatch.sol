// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {B3Types} from "../IB3FinalityProver.sol";
import {B3TypesV2} from "./B3TypesV2.sol";
import {IB3FinalityVerifierV2} from "./IB3FinalityVerifierV2.sol";

/// The release surface of B3VaultV2 that the batch calls.
interface IB3VaultV2Release {
    function released(uint64 withdrawalId) external view returns (bool);

    function release(B3TypesV2.WithdrawalV2 calldata w, bytes32[32] calldata path) external;
}

/// Stateless batch relayer of bridge V2 (doc/design/b3-bridge-v2-contracts-spec.md §9).
///
/// One transaction relays certificates to a B3FinalityVerifierV2 and then
/// releases leaves V2 from a B3VaultV2. Every item is tried on its own: a
/// failing certificate or release emits an event and the batch moves on.
/// Each try forwards at most the caller's per-item gas cap (`certGas`,
/// `releaseGas`), because a malformed BLS witness makes an EIP-2537
/// precompile burn all gas forwarded to it: without a cap one such item
/// would leave the rest of the batch about 1/64 of its gas. With a cap a bad
/// item costs at most its cap. Size the caps from the gas table in
/// contracts/README.md (a 64-member certificate executes in about 5.5M gas, a
/// release in about 0.1M); an item that needs more than its cap fails out of
/// gas and is reported like any other failure. type(uint256).max forwards all
/// available gas (63/64, EIP-150). Certificates run first and in order, so a
/// release may use a root certified earlier in the same batch; with root
/// history (spec §6.4) it may equally use any root accepted before.
///
/// The batch has no constructor, no storage, no owner and no payable entry,
/// and grants nothing: the verifier and the vault are permissionless, so the
/// batch can do nothing a direct call could not. The vault pays each leaf to
/// its recipient, never to the batch. The verifier and vault are arguments,
/// chosen by the caller; a wrong target fails only the caller's own items.
///
/// Not yet here (later slice, with the exit forwarder F_x): spec §9's
/// ExitForward items and the refund of unspent ETH. The batch is not a B3
/// pin, so that slice may extend or replace it.
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

    /// certs[index] reverted with `reason` (the verifier's revert data).
    event CertificateFailed(uint256 indexed index, bytes reason);
    /// releases[index] was not attempted: the vault reports it already released.
    event ReleaseSkipped(uint256 indexed index, uint64 indexed withdrawalId);
    /// releases[index] reverted with `reason` (the vault's revert data).
    event ReleaseFailed(uint256 indexed index, uint64 indexed withdrawalId, bytes reason);

    /// A call to an address without code cannot be caught by try/catch, so a
    /// codeless target named for a non-empty list reverts the whole batch.
    error NoCode(address target);

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
        uint256 certCount = certs.length;
        uint256 releaseCount = releases.length;
        if (certCount != 0 && address(verifier).code.length == 0) revert NoCode(address(verifier));
        if (releaseCount != 0 && vault.code.length == 0) revert NoCode(vault);

        for (uint256 i = 0; i < certCount; ++i) {
            CertInput calldata c = certs[i];
            try verifier.submitCertificate{gas: certGas}(c.fb, c.signingSet, c.successor, c.ancestryPath, c.proof) {
                ++certsAccepted;
            } catch (bytes memory reason) {
                emit CertificateFailed(i, reason);
            }
        }

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
}
