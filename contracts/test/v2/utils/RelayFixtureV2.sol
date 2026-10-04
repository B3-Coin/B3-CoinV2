// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {VmV2} from "../V2TestBase.sol";
import {VerifierFixtureV2} from "./VerifierFixtureV2.sol";
import {BlsTestSignerV2 as Signer} from "./BlsTestSignerV2.sol";
import {MerkleTreeV2} from "./MerkleTreeV2.sol";
import {B3TypesV2} from "../../../src/v2/B3TypesV2.sol";
import {B3FinalityVerifierV2} from "../../../src/v2/B3FinalityVerifierV2.sol";
import {B3VaultV2} from "../../../src/v2/B3VaultV2.sol";
import {IB3FinalityVerifierV2} from "../../../src/v2/IB3FinalityVerifierV2.sol";
import {B3RelayBatch} from "../../../src/v2/B3RelayBatch.sol";
import {MockTetherToken} from "../../../regtest/MockTetherToken.sol";

/// Shared fixture for the suites that run the real V2 stack end to end (test
/// only): BlsDigestProver, B3FinalityVerifierV2, B3VaultV2 on
/// MockTetherToken, and B3RelayBatch. It extends VerifierFixtureV2's chain
/// model (G = 261, 5-block grid, V = 311 at checkpoint 10) with a real
/// withdrawal tree: leaf V2 values for a given vault, their depth-32 root
/// (signed into certificates from V on) and release paths. The leaf at tree
/// position i is withdrawal id i, as in B3's append-only tree.
///
/// The forwarder factory is a predicted, still codeless address (contracts
/// spec §6.1), so only codeless top-level EOAs can deposit here.
abstract contract RelayFixtureV2 is VerifierFixtureV2 {
    B3FinalityVerifierV2.BootstrapMode internal constant K_OF_N = B3FinalityVerifierV2.BootstrapMode.K_OF_N;

    uint256 internal constant UNIT = 1_000_000;
    /// 10,000 USDT in raw units (B-14).
    uint256 internal constant CAP = 10_000_000_000;
    address internal constant FD_FACTORY = address(0xFDFAC7);
    /// Per-item gas caps the fixture passes to B3RelayBatch.relay: above a
    /// 64-member certificate (about 5.4M execution) and a cold release.
    uint256 internal constant CERT_GAS = 6_000_000;
    uint256 internal constant RELEASE_GAS = 250_000;

    address internal constant ALICE = address(0xA11CE0);
    address internal constant BOB = address(0xB0B0B0);
    address internal constant CAROL = address(0xCA201C);
    address internal constant DAVE = address(0xDA7E00);
    /// RECIPIENT_V1: 11 zero bytes, version 1, a 20-byte B3 key hash.
    bytes32 internal constant B3_RECIPIENT =
        bytes32((uint256(1) << 160) | uint256(uint160(0x5151515151515151515151515151515151515151)));

    /// keccak256("Deposit(uint64,address,uint256,bytes32)"), V1's topic 0.
    bytes32 internal constant DEPOSIT_TOPIC = 0xdaf0af297d25c0e96a0b209d35692b4e07c503634eeca57fc5c35c006acf527f;

    MockTetherToken internal usdt;
    B3RelayBatch internal batch;

    function setUp() public virtual override {
        super.setUp();
        usdt = new MockTetherToken();
        batch = new B3RelayBatch();
    }

    // ============================================================ deployment

    /// A profile-R K_OF_N verifier at epoch 0 (Set'0) and its launch vault.
    function deployBridge() internal returns (B3FinalityVerifierV2 v, B3VaultV2 vault) {
        v = deploy(K_OF_N, 0, lineageSet(0).setHash);
        vault = new B3VaultV2(
            B3VaultV2.Config({
                verifier: v,
                verifierCodeHash: address(v).codehash,
                b3ChainDomain: DOMAIN,
                originToken: address(usdt),
                tokenCodeHash: address(usdt).codehash,
                forwarderFactory: FD_FACTORY,
                maxDepositRaw: CAP,
                predecessor: address(0),
                predecessorAssetId: bytes32(0)
            })
        );
    }

    /// 3-of-4 bootstrap keys sign B3's epoch-0 statement at checkpoint
    /// `index` (below V), naming `successor` as Set'1.
    function bootstrap(B3FinalityVerifierV2 v, uint64 index, Signer.TestSet memory successor) internal {
        doInit(v, initArgs(index, bootstrapKeys(), lineageSet(0), successor, Signer.ids(3)));
    }

    // ======================================================== certificates

    /// B3's canonical-shape statement at `index`, with an explicit withdrawal
    /// root and flags, signed by `signer` with `absent` members missing.
    function certWithRoot(
        uint64 branch,
        uint64 index,
        uint64 prevIndex,
        Signer.TestSet memory signer,
        Signer.TestSet memory successor,
        bytes32 withdrawalRoot,
        uint8 bridgeFlags,
        uint32[] memory absent
    ) internal view returns (Cert memory c) {
        c.fb = statement(branch, index, signer.header.epoch, successor.setHash);
        c.fb.withdrawalRoot = withdrawalRoot;
        c.fb.bridgeFlags = bridgeFlags;
        c.signingSet = Signer.copyHeader(signer.header);
        c.successor = Signer.copyHeader(successor.header);
        c.path = ancestry(branch, index, prevIndex);
        c.proof = signFb(signer, c.fb, absent);
    }

    /// As certWithRoot on the canonical branch, flags 0, signed 3-of-4.
    function canonicalCert(
        uint64 index,
        uint64 prevIndex,
        Signer.TestSet memory signer,
        Signer.TestSet memory successor,
        bytes32 withdrawalRoot
    ) internal view returns (Cert memory) {
        return certWithRoot(CANONICAL, index, prevIndex, signer, successor, withdrawalRoot, 0, Signer.ids(3));
    }

    function toInput(Cert memory c) internal pure returns (B3RelayBatch.CertInput memory) {
        return B3RelayBatch.CertInput({
            fb: c.fb, signingSet: c.signingSet, successor: c.successor, ancestryPath: c.path, proof: c.proof
        });
    }

    function inputs(Cert memory a) internal pure returns (B3RelayBatch.CertInput[] memory list) {
        list = new B3RelayBatch.CertInput[](1);
        list[0] = toInput(a);
    }

    function inputs(Cert memory a, Cert memory b, Cert memory c)
        internal
        pure
        returns (B3RelayBatch.CertInput[] memory list)
    {
        list = new B3RelayBatch.CertInput[](3);
        list[0] = toInput(a);
        list[1] = toInput(b);
        list[2] = toInput(c);
    }

    function noCerts() internal pure returns (B3RelayBatch.CertInput[] memory) {
        return new B3RelayBatch.CertInput[](0);
    }

    // ===================================================== withdrawal tree

    function withdrawal(uint64 id, address recipient, uint256 amount, uint64 b3Height)
        internal
        pure
        returns (B3TypesV2.WithdrawalV2 memory)
    {
        return B3TypesV2.WithdrawalV2({withdrawalId: id, recipient: recipient, amount: amount, b3Height: b3Height});
    }

    /// Leaf V2 for `vault`, from its public immutables (spec §6.4).
    function leafOf(B3VaultV2 vault, B3TypesV2.WithdrawalV2 memory w) internal view returns (bytes32) {
        return B3TypesV2.withdrawalLeaf(
            vault.B3_ASSET_ID(),
            w.withdrawalId,
            uint64(block.chainid),
            address(vault),
            vault.ORIGIN_TOKEN(),
            w.recipient,
            w.amount,
            w.b3Height
        );
    }

    /// B3's withdrawal tree over `ws` (ws[i].withdrawalId must be i), all naming `vault`.
    function treeOf(B3VaultV2 vault, B3TypesV2.WithdrawalV2[] memory ws)
        internal
        view
        returns (bytes32[] memory leaves)
    {
        leaves = new bytes32[](ws.length);
        for (uint256 i = 0; i < ws.length; ++i) {
            require(ws[i].withdrawalId == i, "treeOf: id != position");
            leaves[i] = leafOf(vault, ws[i]);
        }
    }

    /// Root of the first `count` leaves; the empty V2 tree root (non-zero) for 0.
    function rootOf(bytes32[] memory leaves, uint256 count) internal pure returns (bytes32) {
        return MerkleTreeV2.root(leaves, count, MerkleTreeV2.HISTORY_DEPTH);
    }

    function emptyTreeRoot() internal pure returns (bytes32) {
        return MerkleTreeV2.root(new bytes32[](0), 0, MerkleTreeV2.HISTORY_DEPTH);
    }

    /// Release of `w` (tree position w.withdrawalId) against the root of the
    /// first `count` leaves. `w` is copied, so altering the item never alters `w`.
    function item(bytes32[] memory leaves, uint256 count, B3TypesV2.WithdrawalV2 memory w)
        internal
        pure
        returns (B3RelayBatch.ReleaseItem memory r)
    {
        r.w = withdrawal(w.withdrawalId, w.recipient, w.amount, w.b3Height);
        r.path = MerkleTreeV2.path32(leaves, count, w.withdrawalId);
    }

    function items(B3RelayBatch.ReleaseItem memory a) internal pure returns (B3RelayBatch.ReleaseItem[] memory list) {
        list = new B3RelayBatch.ReleaseItem[](1);
        list[0] = a;
    }

    function items(B3RelayBatch.ReleaseItem memory a, B3RelayBatch.ReleaseItem memory b)
        internal
        pure
        returns (B3RelayBatch.ReleaseItem[] memory list)
    {
        list = new B3RelayBatch.ReleaseItem[](2);
        list[0] = a;
        list[1] = b;
    }

    function noReleases() internal pure returns (B3RelayBatch.ReleaseItem[] memory) {
        return new B3RelayBatch.ReleaseItem[](0);
    }

    // =============================================================== tokens

    /// Route (a): `eoa` receives `amount` USDT, approves the vault for exactly
    /// that (Tether's zero-first approve rule holds after each full spend) and
    /// deposits it as a codeless top-level EOA.
    function depositAs(B3VaultV2 vault, address eoa, uint256 amount) internal {
        fund(eoa, address(vault), amount);
        vm.prank(eoa, eoa);
        vault.deposit(amount, B3_RECIPIENT);
    }

    function fund(address eoa, address spender, uint256 amount) internal {
        usdt.issue(amount);
        usdt.transfer(eoa, amount);
        vm.prank(eoa);
        usdt.approve(spender, amount);
    }

    // ================================================================ batch

    /// Relays through the batch with the fixture's gas caps and checks it
    /// holds no token and no ETH after the call.
    function relay(
        IB3FinalityVerifierV2 v,
        B3RelayBatch.CertInput[] memory certs,
        address vault,
        B3RelayBatch.ReleaseItem[] memory releases
    ) internal returns (uint256 certsAccepted, uint256 releasesPaid) {
        (certsAccepted, releasesPaid) = batch.relay(v, certs, CERT_GAS, vault, releases, RELEASE_GAS);
        assertBatchEmpty();
    }

    function assertBatchEmpty() internal view {
        assertEq(usdt.balanceOf(address(batch)), 0, "batch holds no token");
        assertEq(address(batch).balance, 0, "batch holds no ETH");
    }

    /// The recorded logs emitted by `emitter`, in order.
    function logsOf(VmV2.Log[] memory all, address emitter) internal pure returns (VmV2.Log[] memory out) {
        uint256 count;
        for (uint256 i = 0; i < all.length; ++i) {
            if (all[i].emitter == emitter) ++count;
        }
        out = new VmV2.Log[](count);
        count = 0;
        for (uint256 i = 0; i < all.length; ++i) {
            if (all[i].emitter == emitter) out[count++] = all[i];
        }
    }

    function assertCertificateFailed(VmV2.Log memory log, uint256 index, bytes memory reason) internal pure {
        assertEq(log.topics.length, 2);
        assertEq(log.topics[0], B3RelayBatch.CertificateFailed.selector);
        assertEq(log.topics[1], bytes32(index));
        assertEq(log.data, abi.encode(reason));
    }

    function assertReleaseSkipped(VmV2.Log memory log, uint256 index, uint64 withdrawalId) internal pure {
        assertEq(log.topics.length, 3);
        assertEq(log.topics[0], B3RelayBatch.ReleaseSkipped.selector);
        assertEq(log.topics[1], bytes32(index));
        assertEq(log.topics[2], bytes32(uint256(withdrawalId)));
        assertEq(log.data.length, 0);
    }

    function assertReleaseFailed(VmV2.Log memory log, uint256 index, uint64 withdrawalId, bytes memory reason)
        internal
        pure
    {
        assertEq(log.topics.length, 3);
        assertEq(log.topics[0], B3RelayBatch.ReleaseFailed.selector);
        assertEq(log.topics[1], bytes32(index));
        assertEq(log.topics[2], bytes32(uint256(withdrawalId)));
        assertEq(log.data, abi.encode(reason));
    }

    function selectorError(bytes4 selector) internal pure returns (bytes memory) {
        return abi.encodeWithSelector(selector);
    }
}
