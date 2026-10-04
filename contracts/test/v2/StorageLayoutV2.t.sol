// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {RelayFixtureV2} from "./utils/RelayFixtureV2.sol";
import {BlsTestSignerV2 as Signer} from "./utils/BlsTestSignerV2.sol";
import {MerkleTreeV2} from "./utils/MerkleTreeV2.sol";
import {B3TypesV2} from "../../src/v2/B3TypesV2.sol";
import {B3FinalityVerifierV2} from "../../src/v2/B3FinalityVerifierV2.sol";
import {B3VaultV2} from "../../src/v2/B3VaultV2.sol";

/// Storage-access cheat codes this suite needs beyond VmV2 (Foundry 1.7.1).
interface VmStorageAccessV2 {
    function record() external;
    function accesses(address target) external returns (bytes32[] memory readSlots, bytes32[] memory writeSlots);
}

/// The normative storage layout that B3 proves by EIP-1186 (contracts spec
/// §3.2 for the verifier, §6.6 for the vault), read back with vm.load and
/// decoded at the documented bit offsets. Packing follows declaration order,
/// first member in the low-order bits.
///
/// Verifier slots (bit offsets in brackets): 0 Liveness {currentEpoch [0],
/// lastRotationTime [64], lastCertificateTime [104], latestBridgeHeight
/// [144], status [208], depositsClosed [216]}; 1 currentSetHash;
/// 2 nextSetHash; 3 SetSummary {currentCount [0], currentWeight [32],
/// nextCount [96], nextWeight [128]}; 4 Latest {height [0], index [64],
/// initTime [128]}; 5 latestBlockHash; 6 latestCertId; 7 latestHistoryRoot;
/// 8 latestBridgeRoot; 9 acceptedRootHeight[root] at keccak(root ‖ 9);
/// 10 setHashByEpoch[epoch] at keccak(u256 epoch ‖ 10).
///
/// Vault slots: 0 nextDepositId (uint64); 1 locked; 2 released[id] at
/// keccak(u256 id ‖ 2); 3 absorbedTotal. The reentrancy lock is transient.
///
/// setUp runs one scenario with a distinct, non-zero value in every packed
/// field: a K_OF_N bootstrap at k = 2 (Set'2, naming a 6-member Set'3), a
/// transition certificate of Set'3 at checkpoint 11 (≥ V) naming a 5-member
/// Set'4 with root R1, two deposits, an absorbed stray transfer, two releases
/// under R1, then a same-epoch certificate at checkpoint 13 with root R2 and
/// bridgeFlags bit 0, so lastCertificateTime differs from lastRotationTime
/// and depositsClosed is set. vm.record captures the slots each call wrote.
contract StorageLayoutV2Test is RelayFixtureV2 {
    VmStorageAccessV2 private constant vmAccess =
        VmStorageAccessV2(address(uint160(uint256(keccak256("hevm cheat code")))));

    // Verifier slots (spec §3.2).
    uint256 private constant LIVENESS_SLOT = 0;
    uint256 private constant CURRENT_SET_HASH_SLOT = 1;
    uint256 private constant NEXT_SET_HASH_SLOT = 2;
    uint256 private constant SET_SUMMARY_SLOT = 3;
    uint256 private constant LATEST_SLOT = 4;
    uint256 private constant LATEST_BLOCK_HASH_SLOT = 5;
    uint256 private constant LATEST_CERT_ID_SLOT = 6;
    uint256 private constant LATEST_HISTORY_ROOT_SLOT = 7;
    uint256 private constant LATEST_BRIDGE_ROOT_SLOT = 8;
    uint256 private constant ACCEPTED_ROOT_HEIGHT_SLOT = 9;
    uint256 private constant SET_HASH_BY_EPOCH_SLOT = 10;
    // Vault slots (spec §6.6).
    uint256 private constant NEXT_DEPOSIT_ID_SLOT = 0;
    uint256 private constant LOCKED_SLOT = 1;
    uint256 private constant RELEASED_SLOT = 2;
    uint256 private constant ABSORBED_TOTAL_SLOT = 3;

    uint256 private constant MASK8 = type(uint8).max;
    uint256 private constant MASK32 = type(uint32).max;
    uint256 private constant MASK40 = type(uint40).max;
    uint256 private constant MASK64 = type(uint64).max;

    // Scenario.
    uint64 private constant K = 2;
    uint64 private constant INIT_INDEX = 3;
    uint64 private constant ROTATE_INDEX = V_INDEX + 1;
    uint64 private constant FLAG_INDEX = V_INDEX + 3;
    uint256 private constant INIT_TIME = T0 + 111;
    uint256 private constant ROTATION_TIME = INIT_TIME + REVIEW;
    uint256 private constant FLAG_TIME = ROTATION_TIME + 333;
    uint32 private constant S2_COUNT = SET_SIZE;
    uint64 private constant S2_WEIGHT = QUALIFIED_WEIGHT;
    uint32 private constant S3_COUNT = 6;
    uint64 private constant S3_WEIGHT = 200;
    uint32 private constant S4_COUNT = 5;
    uint64 private constant S4_WEIGHT = 300;
    uint256 private constant ALICE_DEPOSIT = 700 * UNIT;
    uint256 private constant BOB_DEPOSIT = 400 * UNIT;
    uint256 private constant STRAY = 60 * UNIT;
    /// Withdrawal ids released under R1; ids 0, 2 and 4 stay unreleased.
    uint64 private constant RELEASED_A = 1;
    uint64 private constant RELEASED_B = 3;

    B3FinalityVerifierV2 private v;
    B3VaultV2 private vault;
    bytes32 private s2Hash;
    bytes32 private s3Hash;
    bytes32 private s4Hash;
    bytes32 private root1;
    bytes32 private root2;
    uint256 private expectedLocked;
    B3TypesV2.FinalizedBlockV2 private initFb;
    B3TypesV2.FinalizedBlockV2 private flagFb;

    /// Slot words of the verifier right after initialize (slots 0-8).
    bytes32[9] private afterInit;

    // Storage slots each call wrote, from vm.accesses.
    bytes32[] private initWrites;
    bytes32[] private rotateWrites;
    bytes32[] private flagWrites;
    bytes32[] private depositWrites;
    bytes32[] private absorbWrites;
    bytes32[] private releaseWrites;
    bytes32[] private verifierWritesDuringRelease;

    function setUp() public override {
        super.setUp();
        Signer.TestSet memory s2 = lineageSet(K);
        Signer.TestSet memory s3 =
            Signer.makeUniformSet(harness, K + 1, keccak256("StorageLayoutV2Test/Set'3"), S3_COUNT, S3_WEIGHT);
        Signer.TestSet memory s4 =
            Signer.makeUniformSet(harness, K + 2, keccak256("StorageLayoutV2Test/Set'4"), S4_COUNT, S4_WEIGHT);
        s2Hash = s2.setHash;
        s3Hash = s3.setHash;
        s4Hash = s4.setHash;

        v = deploy(K_OF_N, K, s2.setHash);
        vault = _vaultOn(v);

        // 1. K_OF_N bootstrap at k = 2: 3 of the 4 bootstrap keys sign B3's
        //    epoch-2 statement at checkpoint 3 (below V), naming Set'3.
        vm.warp(INIT_TIME);
        Init memory a = initArgs(INIT_INDEX, bootstrapKeys(), s2, s3, Signer.ids(3));
        initFb = a.fb;
        vmAccess.record();
        doInit(v, a);
        initWrites = _writes(address(v));
        for (uint256 slot = 0; slot < afterInit.length; ++slot) {
            afterInit[slot] = vm.load(address(v), bytes32(slot));
        }

        // B3's withdrawal tree for this vault: ids 0-4, all at height V.
        B3TypesV2.WithdrawalV2[] memory ws = new B3TypesV2.WithdrawalV2[](5);
        ws[0] = withdrawal(0, CAROL, 100 * UNIT, V);
        ws[1] = withdrawal(1, DAVE, 150 * UNIT, V);
        ws[2] = withdrawal(2, CAROL, 200 * UNIT, V);
        ws[3] = withdrawal(3, DAVE, 250 * UNIT, V);
        ws[4] = withdrawal(4, CAROL, 50 * UNIT, V);
        bytes32[] memory leaves = treeOf(vault, ws);
        root1 = rootOf(leaves, 4);
        root2 = rootOf(leaves, 5);

        // 2. After the review window, Set'3 (6 members, 5 signing) takes over
        //    at checkpoint 11 with R1 and names Set'4. Deposits open.
        vm.warp(ROTATION_TIME);
        uint32[] memory absent = Signer.minQuorumAbsent(S3_COUNT);
        Cert memory rotate = certWithRoot(CANONICAL, ROTATE_INDEX, INIT_INDEX, s3, s4, root1, 0, absent);
        vmAccess.record();
        submit(v, rotate);
        rotateWrites = _writes(address(v));
        assertTrue(v.depositsOpen(), "deposits open after the handover");

        // 3. Two deposits in this one transaction (the transient lock clears
        //    between calls), a stray transfer absorbed, two releases under R1.
        fund(ALICE, address(vault), ALICE_DEPOSIT);
        vmAccess.record();
        vm.prank(ALICE, ALICE);
        vault.deposit(ALICE_DEPOSIT, B3_RECIPIENT);
        depositWrites = _writes(address(vault));
        depositAs(vault, BOB, BOB_DEPOSIT);

        usdt.issue(STRAY);
        usdt.transfer(address(vault), STRAY);
        vmAccess.record();
        assertEq(vault.absorb(), STRAY);
        absorbWrites = _writes(address(vault));

        vmAccess.record();
        vault.release(ws[RELEASED_A], MerkleTreeV2.path32(leaves, 4, RELEASED_A));
        releaseWrites = _writes(address(vault));
        verifierWritesDuringRelease = _writes(address(v));
        vault.release(ws[RELEASED_B], MerkleTreeV2.path32(leaves, 4, RELEASED_B));
        expectedLocked = ALICE_DEPOSIT + BOB_DEPOSIT + STRAY - ws[RELEASED_A].amount - ws[RELEASED_B].amount;

        // 4. A same-epoch certificate of Set'3 at checkpoint 13 with R2 and
        //    bridgeFlags bit 0: deposits close for good, the clocks diverge.
        vm.warp(FLAG_TIME);
        Cert memory flag =
            certWithRoot(CANONICAL, FLAG_INDEX, ROTATE_INDEX, s3, s4, root2, B3TypesV2.FLAG_DEPOSITS_CLOSED, absent);
        flagFb = flag.fb;
        vmAccess.record();
        submit(v, flag);
        flagWrites = _writes(address(v));
    }

    // =============================================================== verifier

    function test_BootstrapSlotsAtKEqualsTwo() public view {
        uint256 w0 = uint256(afterInit[LIVENESS_SLOT]);
        assertEq(w0 & MASK64, K, "currentEpoch @0");
        assertEq((w0 >> 64) & MASK40, INIT_TIME, "lastRotationTime @64");
        assertEq((w0 >> 104) & MASK40, 0, "lastCertificateTime @104 stays 0 after bootstrap");
        assertEq((w0 >> 144) & MASK64, 0, "latestBridgeHeight @144 stays 0 (OI-1)");
        assertEq((w0 >> 208) & MASK8, B3TypesV2.STATUS_LIVE, "status @208");
        assertEq((w0 >> 216) & MASK8, 0, "depositsClosed @216");
        assertEq(w0 >> 224, 0, "slot 0 above bit 224");

        assertEq(afterInit[CURRENT_SET_HASH_SLOT], s2Hash, "slot 1 currentSetHash = START_SET_HASH");
        assertEq(afterInit[NEXT_SET_HASH_SLOT], s3Hash, "slot 2 nextSetHash = fb.validatorSetHash");
        _assertSummary(
            uint256(afterInit[SET_SUMMARY_SLOT]), S2_COUNT, S2_COUNT * S2_WEIGHT, S3_COUNT, S3_COUNT * S3_WEIGHT
        );

        uint256 w4 = uint256(afterInit[LATEST_SLOT]);
        assertEq(w4 & MASK64, heightAt(INIT_INDEX), "latest height @0");
        assertEq((w4 >> 64) & MASK64, INIT_INDEX, "latest index @64");
        assertEq((w4 >> 128) & MASK40, INIT_TIME, "initTime @128");
        assertEq(w4 >> 168, 0, "slot 4 above bit 168");

        assertEq(afterInit[LATEST_BLOCK_HASH_SLOT], initFb.blockHash, "slot 5");
        assertEq(afterInit[LATEST_CERT_ID_SLOT], B3TypesV2.certId(initFb), "slot 6");
        assertEq(afterInit[LATEST_HISTORY_ROOT_SLOT], initFb.checkpointHistoryRoot, "slot 7");
        assertEq(afterInit[LATEST_BRIDGE_ROOT_SLOT], bytes32(0), "slot 8: the bootstrap installs no root (OI-1)");
    }

    function test_VerifierSlotsAfterCertificates() public view {
        uint256 w0 = _load(address(v), LIVENESS_SLOT);
        assertEq(w0 & MASK64, K + 1, "currentEpoch @0");
        assertEq((w0 >> 64) & MASK40, ROTATION_TIME, "lastRotationTime @64");
        assertEq((w0 >> 104) & MASK40, FLAG_TIME, "lastCertificateTime @104");
        assertEq((w0 >> 144) & MASK64, heightAt(FLAG_INDEX), "latestBridgeHeight @144");
        assertEq((w0 >> 208) & MASK8, B3TypesV2.STATUS_LIVE, "status @208");
        assertEq((w0 >> 216) & MASK8, 1, "depositsClosed @216");
        assertEq(w0 >> 224, 0, "slot 0 above bit 224");
        // The named getters read the same packed fields.
        assertEq(uint256(v.currentEpoch()), K + 1);
        assertEq(uint256(v.lastRotationTime()), ROTATION_TIME);
        assertEq(uint256(v.lastCertificateTime()), FLAG_TIME);
        assertEq(uint256(v.latestBridgeHeight()), heightAt(FLAG_INDEX));
        assertEq(uint256(v.status()), B3TypesV2.STATUS_LIVE);
        assertTrue(v.depositsClosed());

        assertEq(_word(address(v), CURRENT_SET_HASH_SLOT), s3Hash, "slot 1 currentSetHash");
        assertEq(_word(address(v), NEXT_SET_HASH_SLOT), s4Hash, "slot 2 nextSetHash");
        assertEq(v.currentSetHash(), s3Hash);
        assertEq(v.nextSetHash(), s4Hash);

        uint256 w3 = _load(address(v), SET_SUMMARY_SLOT);
        _assertSummary(w3, S3_COUNT, S3_COUNT * S3_WEIGHT, S4_COUNT, S4_COUNT * S4_WEIGHT);
        (uint32 currentCount, uint64 currentWeight, uint32 nextCount, uint64 nextWeight) = v.setSummary();
        assertEq(uint256(currentCount), w3 & MASK32);
        assertEq(uint256(currentWeight), (w3 >> 32) & MASK64);
        assertEq(uint256(nextCount), (w3 >> 96) & MASK32);
        assertEq(uint256(nextWeight), (w3 >> 128) & MASK64);

        uint256 w4 = _load(address(v), LATEST_SLOT);
        assertEq(w4 & MASK64, heightAt(FLAG_INDEX), "latest height @0");
        assertEq((w4 >> 64) & MASK64, FLAG_INDEX, "latest index @64");
        assertEq((w4 >> 128) & MASK40, INIT_TIME, "initTime @128 survives certificates");
        assertEq(w4 >> 168, 0, "slot 4 above bit 168");
        assertEq(uint256(v.latestHeight()), heightAt(FLAG_INDEX));
        assertEq(uint256(v.latestIndex()), FLAG_INDEX);
        assertEq(uint256(v.initTime()), INIT_TIME);

        assertEq(_word(address(v), LATEST_BLOCK_HASH_SLOT), flagFb.blockHash, "slot 5 latestBlockHash");
        assertEq(_word(address(v), LATEST_CERT_ID_SLOT), B3TypesV2.certId(flagFb), "slot 6 latestCertId");
        assertEq(_word(address(v), LATEST_HISTORY_ROOT_SLOT), flagFb.checkpointHistoryRoot, "slot 7");
        assertEq(_word(address(v), LATEST_BRIDGE_ROOT_SLOT), root2, "slot 8 latestBridgeRoot");
        assertEq(v.latestBlockHash(), flagFb.blockHash);
        assertEq(v.latestCertId(), v.certIdOf(flagFb));
        assertEq(v.latestHistoryRoot(), flagFb.checkpointHistoryRoot);
        assertEq(v.latestBridgeRoot(), root2);
        assertEq(flagFb.checkpointHistoryRoot, historyRoot(CANONICAL, FLAG_INDEX));
    }

    function test_VerifierMappingSlots() public view {
        // acceptedRootHeight[root] at keccak(root ‖ 9), written once per root.
        assertEq(_load(address(v), _acceptedRootSlot(root1)), heightAt(ROTATE_INDEX), "acceptedRootHeight[R1]");
        assertEq(_load(address(v), _acceptedRootSlot(root2)), heightAt(FLAG_INDEX), "acceptedRootHeight[R2]");
        bytes32 never = keccak256("StorageLayoutV2Test/never certified");
        assertEq(_load(address(v), _acceptedRootSlot(never)), 0, "an uncertified root reads 0");
        assertEq(uint256(v.acceptedRootHeight(root1)), heightAt(ROTATE_INDEX));
        assertEq(uint256(v.acceptedRootHeight(root2)), heightAt(FLAG_INDEX));

        // setHashByEpoch[epoch] at keccak(u256 epoch ‖ 10): the start epoch and each rotation.
        assertEq(_word(address(v), _setHashSlot(K)), s2Hash, "setHashByEpoch[2] (bootstrap)");
        assertEq(_word(address(v), _setHashSlot(K + 1)), s3Hash, "setHashByEpoch[3] (rotation)");
        assertEq(_word(address(v), _setHashSlot(K + 2)), bytes32(0), "setHashByEpoch[4] not yet");
        assertEq(_word(address(v), _setHashSlot(K - 1)), bytes32(0), "setHashByEpoch[1] never");
        assertEq(v.setHashByEpoch(K), s2Hash);
        assertEq(v.setHashByEpoch(K + 1), s3Hash);
    }

    /// Each call writes only the normative slots its spec step names, and
    /// every slot it must change: no hidden or reordered state.
    function test_VerifierWritesStayInNormativeSlots() public view {
        // initialize: slots 0-7 and setHashByEpoch[k]; never slot 8 or a root (OI-1).
        bytes32[] memory initAllowed = new bytes32[](9);
        for (uint256 slot = 0; slot < 8; ++slot) {
            initAllowed[slot] = bytes32(slot);
        }
        initAllowed[8] = _setHashSlot(K);
        _assertWritesExactly(initWrites, initAllowed, "initialize");

        // Transition certificate with a new root: slots 0-8, the root and the new epoch.
        bytes32[] memory rotateAllowed = new bytes32[](11);
        for (uint256 slot = 0; slot < 9; ++slot) {
            rotateAllowed[slot] = bytes32(slot);
        }
        rotateAllowed[9] = _acceptedRootSlot(root1);
        rotateAllowed[10] = _setHashSlot(K + 1);
        _assertWritesExactly(rotateWrites, rotateAllowed, "transition certificate");

        // Same-epoch certificate: no set slot (1, 2, 3) and no epoch mapping entry.
        bytes32[] memory flagAllowed = new bytes32[](7);
        flagAllowed[0] = bytes32(LIVENESS_SLOT);
        flagAllowed[1] = bytes32(LATEST_SLOT);
        flagAllowed[2] = bytes32(LATEST_BLOCK_HASH_SLOT);
        flagAllowed[3] = bytes32(LATEST_CERT_ID_SLOT);
        flagAllowed[4] = bytes32(LATEST_HISTORY_ROOT_SLOT);
        flagAllowed[5] = bytes32(LATEST_BRIDGE_ROOT_SLOT);
        flagAllowed[6] = _acceptedRootSlot(root2);
        _assertWritesExactly(flagWrites, flagAllowed, "same-epoch certificate");

        assertEq(verifierWritesDuringRelease.length, 0, "release writes nothing in the verifier");
    }

    // ================================================================== vault

    function test_VaultSlots() public view {
        uint256 w0 = _load(address(vault), NEXT_DEPOSIT_ID_SLOT);
        assertEq(w0 & MASK64, 2, "nextDepositId (uint64) @0");
        assertEq(w0 >> 64, 0, "slot 0 above bit 64");
        assertEq(uint256(vault.nextDepositId()), 2);

        assertEq(_load(address(vault), LOCKED_SLOT), expectedLocked, "slot 1 locked");
        assertEq(vault.locked(), expectedLocked);

        for (uint64 id = 0; id < 5; ++id) {
            bool isReleased = id == RELEASED_A || id == RELEASED_B;
            assertEq(
                _load(address(vault), _releasedSlot(id)),
                isReleased ? uint256(1) : uint256(0),
                "released[id] at keccak(id, 2)"
            );
            assertEq(vault.released(id), isReleased);
        }

        assertEq(_load(address(vault), ABSORBED_TOTAL_SLOT), STRAY, "slot 3 absorbedTotal");
        assertEq(vault.absorbedTotal(), STRAY);
        assertEq(usdt.balanceOf(address(vault)), expectedLocked, "locked equals the balance here");
    }

    /// The `bool transient` lock has no storage slot: nothing but the
    /// normative slots is ever written (a storage lock would be written twice
    /// per call at slot 4), and every slot past 3 reads zero. The two deposits
    /// in setUp share one transaction, so the lock also clears after each call.
    function test_TransientLockLeavesNoStorageFootprint() public view {
        bytes32[] memory depositAllowed = new bytes32[](2);
        depositAllowed[0] = bytes32(NEXT_DEPOSIT_ID_SLOT);
        depositAllowed[1] = bytes32(LOCKED_SLOT);
        _assertWritesExactly(depositWrites, depositAllowed, "deposit");

        bytes32[] memory absorbAllowed = new bytes32[](2);
        absorbAllowed[0] = bytes32(LOCKED_SLOT);
        absorbAllowed[1] = bytes32(ABSORBED_TOTAL_SLOT);
        _assertWritesExactly(absorbWrites, absorbAllowed, "absorb");

        bytes32[] memory releaseAllowed = new bytes32[](2);
        releaseAllowed[0] = bytes32(LOCKED_SLOT);
        releaseAllowed[1] = _releasedSlot(RELEASED_A);
        _assertWritesExactly(releaseWrites, releaseAllowed, "release");

        for (uint256 slot = 4; slot < 32; ++slot) {
            assertEq(_load(address(vault), slot), 0, "vault slot past 3");
        }
        assertEq(uint256(vault.nextDepositId()), 2, "two deposits in one transaction");
    }

    // ================================================================ helpers

    function _vaultOn(B3FinalityVerifierV2 target) private returns (B3VaultV2) {
        return new B3VaultV2(
            B3VaultV2.Config({
                verifier: target,
                verifierCodeHash: address(target).codehash,
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

    function _writes(address target) private returns (bytes32[] memory writeSlots) {
        (, writeSlots) = vmAccess.accesses(target);
    }

    function _assertWritesExactly(bytes32[] memory writes, bytes32[] memory allowed, string memory what) private pure {
        for (uint256 i = 0; i < writes.length; ++i) {
            assertTrue(_contains(allowed, writes[i]), string.concat(what, ": write outside the normative slots"));
        }
        for (uint256 j = 0; j < allowed.length; ++j) {
            assertTrue(_contains(writes, allowed[j]), string.concat(what, ": expected slot not written"));
        }
    }

    function _contains(bytes32[] memory list, bytes32 value) private pure returns (bool) {
        for (uint256 i = 0; i < list.length; ++i) {
            if (list[i] == value) return true;
        }
        return false;
    }

    function _assertSummary(
        uint256 word,
        uint256 currentCount,
        uint256 currentWeight,
        uint256 nextCount,
        uint256 nextWeight
    ) private pure {
        assertEq(word & MASK32, currentCount, "currentCount @0");
        assertEq((word >> 32) & MASK64, currentWeight, "currentWeight @32");
        assertEq((word >> 96) & MASK32, nextCount, "nextCount @96");
        assertEq((word >> 128) & MASK64, nextWeight, "nextWeight @128");
        assertEq(word >> 192, 0, "slot 3 above bit 192");
    }

    function _acceptedRootSlot(bytes32 root) private pure returns (bytes32) {
        return keccak256(abi.encode(root, ACCEPTED_ROOT_HEIGHT_SLOT));
    }

    function _setHashSlot(uint64 epoch) private pure returns (bytes32) {
        return keccak256(abi.encode(epoch, SET_HASH_BY_EPOCH_SLOT));
    }

    function _releasedSlot(uint64 id) private pure returns (bytes32) {
        return keccak256(abi.encode(id, RELEASED_SLOT));
    }

    function _load(address target, uint256 slot) private view returns (uint256) {
        return uint256(vm.load(target, bytes32(slot)));
    }

    function _load(address target, bytes32 slot) private view returns (uint256) {
        return uint256(vm.load(target, slot));
    }

    function _word(address target, uint256 slot) private view returns (bytes32) {
        return vm.load(target, bytes32(slot));
    }

    function _word(address target, bytes32 slot) private view returns (bytes32) {
        return vm.load(target, slot);
    }
}
