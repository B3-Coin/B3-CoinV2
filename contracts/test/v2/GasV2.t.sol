// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {ConsoleV2} from "./V2TestBase.sol";
import {RelayFixtureV2} from "./utils/RelayFixtureV2.sol";
import {BlsTestSignerV2 as Signer} from "./utils/BlsTestSignerV2.sol";
import {MerkleTreeV2} from "./utils/MerkleTreeV2.sol";
import {RuntimeScanV2 as Scan} from "./utils/RuntimeScanV2.sol";
import {B3TypesV2} from "../../src/v2/B3TypesV2.sol";
import {BlsDigestProver} from "../../src/v2/BlsDigestProver.sol";
import {B3FinalityVerifierV2} from "../../src/v2/B3FinalityVerifierV2.sol";
import {B3VaultV2} from "../../src/v2/B3VaultV2.sol";
import {B3RelayBatch} from "../../src/v2/B3RelayBatch.sol";

/// Gas of the V2 launch contracts (contracts spec §11), their EIP-170
/// runtime sizes and the opcodes their runtimes must not contain (spec §12).
///
/// submitCertificate runs on signing sets of 4, 34 and 64 members (weights
/// 250, 100 and 100; all bridge-qualified) at the minimum B3 headcount
/// quorum, so 1, 11 and 21 absent-member witnesses, in three cases:
///
/// - transition, first certificate after the bootstrap: the worst case. It
///   rotates (slots 1-3, a new setHashByEpoch entry, the successor pin),
///   accepts a new root (a new acceptedRootHeight entry) and writes
///   latestBridgeRoot for the first time;
/// - same epoch, new root (steady state);
/// - transition, new root (steady state, one epoch later).
///
/// Also measured: initialize (K_OF_N, 3 of the 4 bootstrap keys), release
/// (cold, MockTetherToken transfer to a fresh recipient), deposit (a second
/// depositor spending its approval) and absorb (a B-23 successor's first
/// absorb after a migration leaf paid it, both slots going from zero).
///
/// Method. setUp prepares every target, so each measured call is the first
/// touch of its contract's storage in the test transaction: the slots carry
/// committed values and SSTORE is priced as in a real transaction
/// (a slot written earlier in the same transaction would be priced as dirty,
/// which under-counts). The target and the prover are cooled with vm.cool,
/// then gasleft() brackets a low-level call of pre-encoded calldata, so the
/// figure includes the cold CALL (a real transaction pre-warms its `to`).
/// The conservative transaction figure is V1's: 21,000 + 40 gas for every
/// calldata byte (EIP-7623's non-zero floor for all bytes) + execution,
/// asserted below the EIP-7825 cap of 2^24. Output shows with -vv.
contract GasV2Test is RelayFixtureV2 {
    uint256 private constant EIP7825_TX_GAS_CAP = 16_777_216;
    uint256 private constant EIP170_RUNTIME_LIMIT = 24_576;
    uint256 private constant EIP3860_INITCODE_LIMIT = 49_152;
    uint256 private constant SIZE_COUNT = 3;

    /// Bootstrap epoch of the certificate bridges: Set_A (epoch 1) starts,
    /// Set_B (epoch 2) and Set_C (epoch 3) follow, all of the case's size.
    uint64 private constant START_EPOCH_K = 1;
    uint64 private constant BOOTSTRAP_INDEX = 1;
    /// Every measurement happens here: past the vault bridge's review window
    /// (deposits open) and within every certificate bridge's windows.
    uint256 private constant MEASURE_TIME = T0 + REVIEW + 200;

    uint256 private constant ALICE_DEPOSIT = 1_000 * UNIT;
    uint256 private constant BOB_DEPOSIT = 400 * UNIT;
    uint256 private constant MIGRATION_AMOUNT = 200 * UNIT;
    uint256 private constant PAYEE_AMOUNT = 300 * UNIT;
    address private constant FRESH_PAYEE = address(0xFEE0FEE0);

    // Certificate bridges, one per size, each ready for its measured certificate.
    B3FinalityVerifierV2[SIZE_COUNT] private firstV;
    B3FinalityVerifierV2[SIZE_COUNT] private sameEpochV;
    B3FinalityVerifierV2[SIZE_COUNT] private transitionV;
    bytes[SIZE_COUNT] private firstCall;
    bytes[SIZE_COUNT] private sameEpochCall;
    bytes[SIZE_COUNT] private transitionCall;
    bytes32[SIZE_COUNT] private setDHash;

    // Bootstrap target.
    B3FinalityVerifierV2 private initV;
    bytes private initCall;

    // Vault bridge.
    B3FinalityVerifierV2 private vaultV;
    B3VaultV2 private vault;
    B3VaultV2 private successor;
    bytes private releaseCall;
    bytes private depositCall;

    function setUp() public override {
        super.setUp();

        // 1. At T0: every certificate bridge and the vault bridge bootstrap.
        Signer.TestSet memory s1 = lineageSet(1);
        (vaultV, vault) = deployBridge();
        bootstrap(vaultV, 1, s1);
        for (uint256 i = 0; i < SIZE_COUNT; ++i) {
            _prepareSize(i);
        }

        // 2. At T0 + MIN_EPOCH: the first transition lands on the same-epoch
        //    and transition bridges, the same-epoch certificate on the latter.
        vm.warp(T0 + MIN_EPOCH);
        for (uint256 i = 0; i < SIZE_COUNT; ++i) {
            _call(address(sameEpochV[i]), firstCall[i]);
            _call(address(transitionV[i]), firstCall[i]);
            _call(address(transitionV[i]), sameEpochCall[i]);
        }

        // 3. At MEASURE_TIME: Set'1 takes over the vault bridge at V with a
        //    real root over two leaves V2 of the launch vault, one paying a
        //    fresh payee and one a B-23 successor (the migration leaf).
        vm.warp(MEASURE_TIME);
        successor = new B3VaultV2(
            B3VaultV2.Config({
                verifier: vaultV,
                verifierCodeHash: address(vaultV).codehash,
                b3ChainDomain: DOMAIN,
                originToken: address(usdt),
                tokenCodeHash: address(usdt).codehash,
                forwarderFactory: FD_FACTORY,
                maxDepositRaw: CAP,
                predecessor: address(vault),
                predecessorAssetId: vault.B3_ASSET_ID()
            })
        );
        B3TypesV2.WithdrawalV2[] memory ws = new B3TypesV2.WithdrawalV2[](2);
        ws[0] = withdrawal(0, FRESH_PAYEE, PAYEE_AMOUNT, V);
        ws[1] = withdrawal(1, address(successor), MIGRATION_AMOUNT, V);
        bytes32[] memory leaves = treeOf(vault, ws);
        submit(vaultV, canonicalCert(V_INDEX, 1, s1, lineageSet(2), rootOf(leaves, 2)));
        assertTrue(vault.depositGateOpen(), "vault bridge open");

        depositAs(vault, ALICE, ALICE_DEPOSIT);
        vault.release(ws[1], MerkleTreeV2.path32(leaves, 2, 1));
        assertEq(usdt.balanceOf(address(successor)), MIGRATION_AMOUNT);
        fund(BOB, address(vault), BOB_DEPOSIT);
        releaseCall = abi.encodeCall(B3VaultV2.release, (ws[0], MerkleTreeV2.path32(leaves, 2, 0)));
        depositCall = abi.encodeCall(B3VaultV2.deposit, (BOB_DEPOSIT, B3_RECIPIENT));

        // 4. A fresh K_OF_N verifier (epoch 0) awaiting its bootstrap.
        initV = deploy(K_OF_N, 0, lineageSet(0).setHash);
        Init memory a = initArgs(initV, BOOTSTRAP_INDEX, bootstrapKeys(), lineageSet(0), s1, Signer.ids(3));
        initCall = abi.encodeCall(
            B3FinalityVerifierV2.initialize, (a.fb, a.signingSet, a.startSet, a.successor, a.leaf0Path, a.proof)
        );
    }

    // =================================================== submitCertificate

    function test_GasSubmitCertificate_FirstTransitionAfterBootstrap() public {
        assertEq(block.timestamp, MEASURE_TIME);
        for (uint256 i = 0; i < SIZE_COUNT; ++i) {
            B3FinalityVerifierV2 target = firstV[i];
            (uint256 execution, uint256 conservative) = _measureCertificate(target, firstCall[i]);
            assertEq(uint256(target.currentEpoch()), START_EPOCH_K + 1);
            assertEq(uint256(target.latestIndex()), V_INDEX);
            assertEq(uint256(target.acceptedRootHeight(withdrawalRootAt(CANONICAL, V_INDEX))), V);
            _report(
                "submitCertificate, transition, first after bootstrap", i, firstCall[i].length, execution, conservative
            );
        }
    }

    function test_GasSubmitCertificate_SameEpochNewRoot() public {
        for (uint256 i = 0; i < SIZE_COUNT; ++i) {
            B3FinalityVerifierV2 target = sameEpochV[i];
            (uint256 execution, uint256 conservative) = _measureCertificate(target, sameEpochCall[i]);
            assertEq(uint256(target.currentEpoch()), START_EPOCH_K + 1);
            assertEq(uint256(target.latestIndex()), V_INDEX + 1);
            assertEq(
                uint256(target.acceptedRootHeight(withdrawalRootAt(CANONICAL, V_INDEX + 1))), heightAt(V_INDEX + 1)
            );
            _report("submitCertificate, same epoch, new root", i, sameEpochCall[i].length, execution, conservative);
        }
    }

    function test_GasSubmitCertificate_TransitionNewRoot() public {
        for (uint256 i = 0; i < SIZE_COUNT; ++i) {
            B3FinalityVerifierV2 target = transitionV[i];
            (uint256 execution, uint256 conservative) = _measureCertificate(target, transitionCall[i]);
            assertEq(uint256(target.currentEpoch()), START_EPOCH_K + 2);
            assertEq(uint256(target.latestIndex()), V_INDEX + 2);
            assertEq(target.nextSetHash(), setDHash[i]);
            assertEq(
                uint256(target.acceptedRootHeight(withdrawalRootAt(CANONICAL, V_INDEX + 2))), heightAt(V_INDEX + 2)
            );
            _report("submitCertificate, transition, new root", i, transitionCall[i].length, execution, conservative);
        }
    }

    // ============================================== bootstrap and vault

    function test_GasInitializeKOfN() public {
        vm.cool(address(initV));
        vm.cool(address(prover));
        (uint256 execution, uint256 conservative) = _measure(address(initV), initCall);
        assertEq(initV.status(), B3TypesV2.STATUS_LIVE);
        _log("initialize, K_OF_N (3 of 4 bootstrap keys)", initCall.length, execution, conservative);
    }

    function test_GasRelease() public {
        vm.cool(address(vault));
        vm.cool(address(vaultV));
        vm.cool(address(usdt));
        (uint256 execution, uint256 conservative) = _measure(address(vault), releaseCall);
        assertTrue(vault.released(0));
        assertEq(usdt.balanceOf(FRESH_PAYEE), PAYEE_AMOUNT);
        assertEq(vault.locked(), ALICE_DEPOSIT - MIGRATION_AMOUNT - PAYEE_AMOUNT);
        _log("release (cold, fresh recipient)", releaseCall.length, execution, conservative);
    }

    function test_GasDeposit() public {
        vm.cool(address(vault));
        vm.cool(address(vaultV));
        vm.cool(address(usdt));
        vm.prank(BOB, BOB);
        (uint256 execution, uint256 conservative) = _measure(address(vault), depositCall);
        assertEq(uint256(vault.nextDepositId()), 2);
        assertEq(vault.locked(), ALICE_DEPOSIT - MIGRATION_AMOUNT + BOB_DEPOSIT);
        _log("deposit (second depositor, USDT allowance spent)", depositCall.length, execution, conservative);
    }

    function test_GasAbsorb() public {
        bytes memory absorbCall = abi.encodeCall(B3VaultV2.absorb, ());
        vm.cool(address(successor));
        vm.cool(address(usdt));
        (uint256 execution, uint256 conservative) = _measure(address(successor), absorbCall);
        assertEq(successor.locked(), MIGRATION_AMOUNT);
        assertEq(successor.absorbedTotal(), MIGRATION_AMOUNT);
        _log("absorb (successor's first, after a migration leaf)", absorbCall.length, execution, conservative);

        // A second absorb finds nothing to add (reads only, so cooling is exact).
        vm.cool(address(successor));
        vm.cool(address(usdt));
        (execution, conservative) = _measure(address(successor), absorbCall);
        assertEq(successor.absorbedTotal(), MIGRATION_AMOUNT);
        _log("absorb (no-op)", absorbCall.length, execution, conservative);
    }

    // ================================================================ sizes

    function test_RuntimeSizesWithinEip170() public view {
        _assertSize("BlsDigestProver", address(prover).code.length, type(BlsDigestProver).creationCode.length);
        _assertSize("B3FinalityVerifierV2", address(vaultV).code.length, type(B3FinalityVerifierV2).creationCode.length);
        _assertSize("B3VaultV2", address(vault).code.length, type(B3VaultV2).creationCode.length);
        _assertSize("B3RelayBatch", address(batch).code.length, type(B3RelayBatch).creationCode.length);
    }

    /// Spec §12 (check-no-number-opcode.sh, deferred): no V2 runtime reads
    /// NUMBER, so each can later run on Arbitrum. None delegates, creates or
    /// self-destructs; the prover and the batch never write storage.
    function test_RuntimesHaveNoNumberOpcode() public view {
        _assertOpcodes("BlsDigestProver", address(prover).code, false);
        _assertOpcodes("B3FinalityVerifierV2", address(vaultV).code, true);
        _assertOpcodes("B3VaultV2", address(vault).code, true);
        _assertOpcodes("B3RelayBatch", address(batch).code, false);
    }

    // ============================================================== helpers

    function _assertOpcodes(string memory name, bytes memory code, bool writesStorage) private pure {
        (uint256[256] memory counts, uint256 total) = Scan.opcodeCounts(code);
        assertGt(total, 100);
        assertEq(counts[Scan.NUMBER], 0, name);
        assertEq(counts[Scan.DELEGATECALL] + counts[Scan.CALLCODE], 0, name);
        assertEq(counts[Scan.CREATE] + counts[Scan.CREATE2] + counts[Scan.SELFDESTRUCT], 0, name);
        if (writesStorage) {
            assertGt(counts[Scan.SSTORE], 0);
        } else {
            assertEq(counts[Scan.SSTORE] + counts[Scan.TSTORE], 0, name);
        }
    }

    function _n(uint256 i) private pure returns (uint32) {
        return i == 0 ? 4 : (i == 1 ? 34 : 64);
    }

    function _weight(uint256 i) private pure returns (uint64) {
        return i == 0 ? 250 : 100;
    }

    /// Three bridges of one size bootstrapped at T0 (K_OF_N at epoch 1 with
    /// Set_A, naming Set_B), and the three measured certificates:
    /// Set_B's transition at V naming Set_C, Set_B's same-epoch certificate
    /// at checkpoint V+1 and Set_C's transition at V+2 naming Set'4.
    function _prepareSize(uint256 i) private {
        uint32 n = _n(i);
        uint64 weight = _weight(i);
        Signer.TestSet memory setA =
            Signer.makeUniformSet(harness, START_EPOCH_K, keccak256(abi.encode("GasV2Test/Set_A", n)), n, weight);
        Signer.TestSet memory setB =
            Signer.makeUniformSet(harness, START_EPOCH_K + 1, keccak256(abi.encode("GasV2Test/Set_B", n)), n, weight);
        Signer.TestSet memory setC =
            Signer.makeUniformSet(harness, START_EPOCH_K + 2, keccak256(abi.encode("GasV2Test/Set_C", n)), n, weight);
        Signer.TestSet memory setD = lineageSet(START_EPOCH_K + 3);
        setDHash[i] = setD.setHash;

        uint32[] memory absent = Signer.minQuorumAbsent(n);
        assertEq(absent.length, n - (uint256(n) * 2 / 3 + 1));

        firstCall[i] = _encode(
            certWithRoot(
                CANONICAL, V_INDEX, BOOTSTRAP_INDEX, setB, setC, withdrawalRootAt(CANONICAL, V_INDEX), 0, absent
            )
        );
        sameEpochCall[i] = _encode(
            certWithRoot(
                CANONICAL, V_INDEX + 1, V_INDEX, setB, setC, withdrawalRootAt(CANONICAL, V_INDEX + 1), 0, absent
            )
        );
        transitionCall[i] = _encode(
            certWithRoot(
                CANONICAL, V_INDEX + 2, V_INDEX + 1, setC, setD, withdrawalRootAt(CANONICAL, V_INDEX + 2), 0, absent
            )
        );

        firstV[i] = deploy(K_OF_N, START_EPOCH_K, setA.setHash);
        sameEpochV[i] = deploy(K_OF_N, START_EPOCH_K, setA.setHash);
        transitionV[i] = deploy(K_OF_N, START_EPOCH_K, setA.setHash);
        // OI-12: an attestation names its verifier, so each deployment gets its own.
        Signer.TestSet memory keys = bootstrapKeys();
        doInit(firstV[i], initArgs(firstV[i], BOOTSTRAP_INDEX, keys, setA, setB, Signer.ids(3)));
        doInit(sameEpochV[i], initArgs(sameEpochV[i], BOOTSTRAP_INDEX, keys, setA, setB, Signer.ids(3)));
        doInit(transitionV[i], initArgs(transitionV[i], BOOTSTRAP_INDEX, keys, setA, setB, Signer.ids(3)));
    }

    function _encode(Cert memory c) private pure returns (bytes memory) {
        return
            abi.encodeCall(B3FinalityVerifierV2.submitCertificate, (c.fb, c.signingSet, c.successor, c.path, c.proof));
    }

    function _call(address target, bytes memory data) private {
        (bool ok, bytes memory ret) = target.call(data);
        if (!ok) {
            assembly {
                revert(add(ret, 0x20), mload(ret))
            }
        }
    }

    function _measureCertificate(B3FinalityVerifierV2 target, bytes memory data)
        private
        returns (uint256 execution, uint256 conservative)
    {
        vm.cool(address(target));
        vm.cool(address(prover));
        return _measure(address(target), data);
    }

    /// Execution gas of one call of `data` to `target` (bubbling a revert),
    /// and V1's conservative transaction total, asserted under the EIP-7825 cap.
    function _measure(address target, bytes memory data) private returns (uint256 execution, uint256 conservative) {
        uint256 g0 = gasleft();
        (bool ok, bytes memory ret) = target.call(data);
        execution = g0 - gasleft();
        if (!ok) {
            assembly {
                revert(add(ret, 0x20), mload(ret))
            }
        }
        conservative = 21_000 + data.length * 40 + execution;
        assertLt(conservative, EIP7825_TX_GAS_CAP, "EIP-7825 transaction gas cap");
    }

    function _report(string memory what, uint256 i, uint256 calldataBytes, uint256 execution, uint256 conservative)
        private
        view
    {
        uint256 n = _n(i);
        string memory head =
            string.concat(what, " [n=", vm.toString(n), ", ", vm.toString(n - (n * 2 / 3 + 1)), " absent]");
        _log(head, calldataBytes, execution, conservative);
    }

    function _log(string memory head, uint256 calldataBytes, uint256 execution, uint256 conservative) private view {
        ConsoleV2.log(string.concat(head, ": calldata bytes"), calldataBytes);
        ConsoleV2.log(string.concat(head, ": execution gas"), execution);
        ConsoleV2.log(string.concat(head, ": conservative tx gas"), conservative);
    }

    function _assertSize(string memory name, uint256 runtime, uint256 initcode) private view {
        ConsoleV2.log(string.concat(name, ": runtime bytes"), runtime);
        ConsoleV2.log(string.concat(name, ": initcode bytes"), initcode);
        assertGt(runtime, 0);
        assertLe(runtime, EIP170_RUNTIME_LIMIT, string.concat(name, ": EIP-170 runtime limit"));
        assertLe(initcode, EIP3860_INITCODE_LIMIT, string.concat(name, ": EIP-3860 initcode limit"));
    }
}
