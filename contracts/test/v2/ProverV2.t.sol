// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {V2TestBase, ConsoleV2} from "./V2TestBase.sol";
import {BlsTestSignerV2 as Signer, BlsDigestProverHarnessV2} from "./utils/BlsTestSignerV2.sol";
import {B3Types} from "../../src/IB3FinalityProver.sol";
import {BlsCertificateProver} from "../../src/BlsCertificateProver.sol";
import {B3TypesV2} from "../../src/v2/B3TypesV2.sol";
import {BlsDigestProver} from "../../src/v2/BlsDigestProver.sol";

/// BlsDigestProver (contracts spec §3.6).
///
/// Part 1 is the independent cross-language check: the committed C++ vectors
/// (B3's vendored blst v0.3.17; test/BlsCertificateVector.t.sol,
/// test/vectors/bls64_proof.bin and bls64_bootstrap_proof.bin, all unedited)
/// sign V1 digests. A BLS signature covers only the 32-byte message, so the
/// same witnesses must verify through verifyDigest with that digest and the
/// same header re-labelled rulesetVersion = 2, and must fail under ruleset 1,
/// another digest or a tampered witness. Part 2 drives the in-EVM signer
/// (test/v2/utils/BlsTestSignerV2.sol) through the dual quorum and every
/// witness rule. Part 3 shows that a V1 proof cannot pass under digest V2.
///
/// Calls that may hit a failing EIP-2537 precompile (invalid point encodings)
/// carry a gas cap, because such a failure burns all gas forwarded to it.
contract ProverV2Test is V2TestBase {
    uint256 private constant VERIFY_GAS = 20_000_000;

    // ---- Committed 1-member blst vector (test/BlsCertificateVector.t.sol).
    bytes32 private constant D0_DOMAIN = hex"d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0";
    bytes32 private constant FILL_01 = hex"0101010101010101010101010101010101010101010101010101010101010101";
    bytes32 private constant FILL_02 = hex"0202020202020202020202020202020202020202020202020202020202020202";
    bytes32 private constant FILL_03 = hex"0303030303030303030303030303030303030303030303030303030303030303";
    bytes32 private constant FILL_11 = hex"1111111111111111111111111111111111111111111111111111111111111111";
    bytes32 private constant B3_BLST_DIGEST = hex"c02114826d2f2199a6bd2e186e51e25becd786c9a1b7464e7230cd76df098a65";
    bytes private constant ONE_COMPRESSED_PUBLIC_KEY =
        hex"aa3b6ee8aadb63c8e064dafdc3d0ae794ded037af26e60829da1592db0a237dc42684307339f176c31d97714fd78aed2";
    bytes private constant ONE_EIP2537_PUBLIC_KEY =
        hex"000000000000000000000000000000000a3b6ee8aadb63c8e064dafdc3d0ae794ded037af26e60829da1592db0a237dc42684307339f176c31d97714fd78aed2000000000000000000000000000000000d4611741ecb196805e3694d993b438dc6375d97e16430a57bf908c38482d9370a598e77552632acda8dec2033a7e4fc";
    bytes private constant ONE_EIP2537_SIGNATURE =
        hex"0000000000000000000000000000000018f90a30ac5823ea45032e3e173bcc0e7d76d8aa4d59aff19492f15bae98a0d7adb1cd895adc4b804718a4138cee4c15000000000000000000000000000000000f9775cab5513e55c48bb1bf625ca72be82529a074b01e461a020b0fe84b5a53eec6875b317f56febab3f84db1cc624b0000000000000000000000000000000004bca93e2a4de8c020d0a072e5ac55ede7e96da6c323d00b818a58196d9e33961a6e9a4508d5030c6b74f0fa02c69fcc0000000000000000000000000000000002758dc377d43dfe212ef15f1c18df6a8b3e87d34de3cbcd3375c8e52ee86bafe46757bf51018d349a44ce7eac8d528f";

    // ---- Committed 64-member and 4-member blst vectors (test/vectors/README.md).
    string private constant BLS64_PROOF_PATH = "test/vectors/bls64_proof.bin";
    string private constant BLS64_BOOTSTRAP_PROOF_PATH = "test/vectors/bls64_bootstrap_proof.bin";
    bytes32 private constant BLS64_PROOF_SHA256 = hex"a77a685dbdc9690877ff34265397a13e4d440a123ea56b6005315b627ae8178e";
    bytes32 private constant BLS64_BOOTSTRAP_SHA256 =
        hex"a1f5e2e103c52a9f69595ed0cbea72810a2ec6bc9a49e9efac180660c0182870";
    /// V1 (ruleset-1) hashes of the C++ Set_0 and Set_1; the signed statements name them.
    bytes32 private constant SET0_HASH_V1 = hex"23c41c0c48be76ff3983e6e94c460de4c67584414d3c89f911c336b31a396d6c";
    bytes32 private constant SET1_HASH_V1 = hex"484039e5e8fe8a24c83aa52af44e284f22d5a9d12c2f0053c3ce862c72a6a090";
    bytes32 private constant BOOTSTRAP_SET_HASH_V1 =
        hex"5188225d2e128c1f51a61f4f6bad92dd8da6566549156bae3ebb81728158bf14";
    bytes private constant SET64_AGGREGATE_PUBLIC_KEY =
        hex"936e276e891a36681c10868bd616382743a374877d4e36f1fb508c40122885f81cfb75e0395547819b94e35caff5e76e";
    bytes32 private constant SET64_MEMBERS_ROOT = hex"68adb90eed00952dd48e44f440f9f4e0d1be569f944757226883ff3c6a461124";
    bytes private constant BOOTSTRAP_AGGREGATE_PUBLIC_KEY =
        hex"afea1502f003a9ab0d8bf89c1826bc78d3e01413f9e78e836b0b99dba05c3ce5d9e81f1478697a4d776aeee83449ec54";
    bytes32 private constant BOOTSTRAP_MEMBERS_ROOT =
        hex"78621e43cecacca419e7d29cb7458f31262616dce6083cf3f32ddec1b67b5910";

    bytes32 private constant SEED_A = keccak256("ProverV2Test/seed A");
    bytes32 private constant SEED_B = keccak256("ProverV2Test/seed B");

    BlsDigestProver private prover;
    BlsCertificateProver private proverV1;
    BlsDigestProverHarnessV2 private harness;

    function setUp() public {
        prover = new BlsDigestProver();
        proverV1 = new BlsCertificateProver();
        harness = new BlsDigestProverHarnessV2();
    }

    // =====================================================================
    // Part 1: committed C++ (blst) vectors through verifyDigest
    // =====================================================================

    function test_Blst1MemberVectorVerifiesUnderRuleset2() public view {
        bytes32 digest = _v1Digest(D0_DOMAIN, 811_001, FILL_01, FILL_02, FILL_03, 7);
        assertEq(digest, B3_BLST_DIGEST, "C++ V1 digest");

        B3Types.SetHeader memory set2 = _oneMemberSet(B3TypesV2.RULESET_V2);
        bytes memory proof = abi.encode(hex"01", ONE_EIP2537_SIGNATURE, ONE_EIP2537_PUBLIC_KEY, _noAbsent());
        assertTrue(_verify(digest, _hash(set2), set2, proof), "blst 1-member, ruleset 2");

        // The same witness under a ruleset-1 header: V1 accepts, V2 refuses.
        B3Types.SetHeader memory set1 = _oneMemberSet(1);
        assertTrue(
            proverV1.verify(D0_DOMAIN, _v1Block(811_001, FILL_01, FILL_02, FILL_03, 7), _hash(set1), set1, proof),
            "V1 prover accepts its own vector"
        );
        assertFalse(_verify(digest, _hash(set1), set1, proof), "ruleset 1 refused");

        // Pairing, not a live precompile: another message fails.
        assertFalse(_verify(_flip(digest), _hash(set2), set2, proof), "changed digest");

        // A byte-flipped signature is not a G2 point; the pairing call fails closed.
        bytes memory badSignature = bytes.concat(ONE_EIP2537_SIGNATURE);
        badSignature[255] = bytes1(uint8(badSignature[255]) ^ 0x01);
        assertFalse(
            _verify(digest, _hash(set2), set2, abi.encode(hex"01", badSignature, ONE_EIP2537_PUBLIC_KEY, _noAbsent())),
            "tampered signature"
        );

        // blst's compression equals the harness's (used by the in-EVM signer).
        assertEq(harness.compressG1(ONE_EIP2537_PUBLIC_KEY), ONE_COMPRESSED_PUBLIC_KEY, "blst compression");
    }

    function test_Blst64MemberVectorVerifiesUnderRuleset2() public view {
        bytes memory proof = vm.readFileBinary(BLS64_PROOF_PATH);
        assertEq(sha256(proof), BLS64_PROOF_SHA256, "fixture hash");
        bytes32 digest = _v1Digest(D0_DOMAIN, 811_001, FILL_01, FILL_02, SET1_HASH_V1, 0);

        B3Types.SetHeader memory set2 = _set64(B3TypesV2.RULESET_V2);
        B3Types.SetHeader memory set1 = _set64(1);
        assertEq(_hash(set1), SET0_HASH_V1, "C++ Set_0 header (ruleset 1)");

        uint256 gasBefore = gasleft();
        bool ok = prover.verifyDigest{gas: VERIFY_GAS}(digest, _hash(set2), set2, proof);
        uint256 v2Gas = gasBefore - gasleft();
        assertTrue(ok, "blst 64-member 43-signer, ruleset 2");

        gasBefore = gasleft();
        ok = proverV1.verify{gas: VERIFY_GAS}(
            D0_DOMAIN, _v1Block(811_001, FILL_01, FILL_02, SET1_HASH_V1, 0), SET0_HASH_V1, set1, proof
        );
        uint256 v1Gas = gasBefore - gasleft();
        assertTrue(ok, "V1 prover accepts its own vector");
        ConsoleV2.log("bls64 verifyDigest gas (V2)", v2Gas);
        ConsoleV2.log("bls64 verify gas (V1)", v1Gas);

        assertFalse(_verify(digest, SET0_HASH_V1, set1, proof), "ruleset 1 refused");
        assertFalse(_verify(_flip(digest), _hash(set2), set2, proof), "changed digest");
        assertFalse(_verify(digest, _hash(set1), set2, proof), "set hash of another header");
        assertFalse(_verify(digest, bytes32(0), set2, proof), "zero set hash");

        Signer.Parts memory p = _decode(proof);
        assertEq(p.absent.length, 21);

        // Tampered signature (byte flip: no longer a G2 point).
        Signer.Parts memory t = _decode(proof);
        t.signature[200] = bytes1(uint8(t.signature[200]) ^ 0x01);
        assertFalse(_verify(digest, _hash(set2), set2, Signer.encode(t)), "tampered signature");

        // Tampered absent key: another absent member's key at index 43.
        t = _decode(proof);
        t.absent[0].pubkey = p.absent[1].pubkey;
        t.absent[0].uncompressedPubkey = p.absent[1].uncompressedPubkey;
        assertFalse(_verify(digest, _hash(set2), set2, Signer.encode(t)), "absent key swapped");

        // Compressed and uncompressed absent keys disagree.
        t = _decode(proof);
        t.absent[0].uncompressedPubkey = p.absent[1].uncompressedPubkey;
        assertFalse(_verify(digest, _hash(set2), set2, Signer.encode(t)), "absent key witness mismatch");

        // Absent weight understated (the member leaf commits the weight).
        t = _decode(proof);
        t.absent[20].weight = 0;
        assertFalse(_verify(digest, _hash(set2), set2, Signer.encode(t)), "absent weight");

        // One absent member dropped and claimed as a signer it never was.
        t = _decode(proof);
        t.bitmap[5] = bytes1(uint8(t.bitmap[5]) | 0x08); // index 43
        t.absent = _dropFirst(t.absent);
        assertFalse(_verify(digest, _hash(set2), set2, Signer.encode(t)), "absent claimed as signer");
    }

    /// The C++ 3-of-4 bootstrap handoff: the shape of the K_OF_N bootstrap.
    function test_Blst4MemberBootstrapVector3of4UnderRuleset2() public view {
        bytes memory proof = vm.readFileBinary(BLS64_BOOTSTRAP_PROOF_PATH);
        assertEq(sha256(proof), BLS64_BOOTSTRAP_SHA256, "fixture hash");
        bytes32 digest = _v1Digest(D0_DOMAIN, 811_000, FILL_11, bytes32(0), SET0_HASH_V1, 0);

        B3Types.SetHeader memory boot2 = _bootstrapSet(B3TypesV2.RULESET_V2);
        B3Types.SetHeader memory boot1 = _bootstrapSet(1);
        assertEq(_hash(boot1), BOOTSTRAP_SET_HASH_V1, "C++ bootstrap header (ruleset 1)");

        assertTrue(_verify(digest, _hash(boot2), boot2, proof), "blst 3-of-4, ruleset 2");
        assertFalse(_verify(digest, _hash(boot1), boot1, proof), "ruleset 1 refused");
        assertFalse(_verify(_flip(digest), _hash(boot2), boot2, proof), "changed digest");

        // Only three of four signed: claiming the fourth as a signer fails.
        Signer.Parts memory t = _decode(proof);
        t.bitmap[0] = 0x0f;
        t.absent = new BlsDigestProver.Absent[](0);
        assertFalse(_verify(digest, _hash(boot2), boot2, Signer.encode(t)), "fourth signer claimed");
    }

    /// blst and the harness agree on G1 compression for every committed key,
    /// and each committed absent path opens the committed members root.
    function test_BlstKeysAgreeWithHarnessCompressionAndMemberPaths() public view {
        Signer.Parts memory p = _decode(vm.readFileBinary(BLS64_PROOF_PATH));
        assertEq(harness.compressG1(p.aggregate), SET64_AGGREGATE_PUBLIC_KEY, "64-member aggregate");
        uint256 withYFlag;
        for (uint256 i = 0; i < p.absent.length; ++i) {
            BlsDigestProver.Absent memory a = p.absent[i];
            assertEq(harness.compressG1(a.uncompressedPubkey), a.pubkey, "absent key compression");
            assertEq(
                prover.validatorMemberRoot(a.index, a.pubkey, a.weight, a.siblings),
                SET64_MEMBERS_ROOT,
                "absent member path"
            );
            if (uint8(a.pubkey[0]) & 0x20 != 0) ++withYFlag;
        }
        // Both sign-flag values occur, so compression is checked on each branch.
        assertTrue(withYFlag != 0 && withYFlag != p.absent.length, "both y flags covered");

        Signer.Parts memory b = _decode(vm.readFileBinary(BLS64_BOOTSTRAP_PROOF_PATH));
        assertEq(harness.compressG1(b.aggregate), BOOTSTRAP_AGGREGATE_PUBLIC_KEY, "bootstrap aggregate");
        assertEq(
            prover.validatorMemberRoot(b.absent[0].index, b.absent[0].pubkey, b.absent[0].weight, b.absent[0].siblings),
            BOOTSTRAP_MEMBERS_ROOT,
            "bootstrap absent path"
        );
    }

    // =====================================================================
    // Part 3: a V1 proof fails under digest V2
    // =====================================================================

    /// The C++ 64-member V1 proof, presented for the V2 statement carrying
    /// the same height, hash, root, successor and epoch.
    function test_V1ProofFailsUnderDigestV2_Blst64() public view {
        bytes memory proof = vm.readFileBinary(BLS64_PROOF_PATH);
        B3TypesV2.FinalizedBlockV2 memory fb = _statementV2(811_001, FILL_01, FILL_02, SET1_HASH_V1, 0);
        bytes32 digestV2 = B3TypesV2.digest(D0_DOMAIN, fb);
        assertNotEq(digestV2, _v1Digest(D0_DOMAIN, 811_001, FILL_01, FILL_02, SET1_HASH_V1, 0));

        B3Types.SetHeader memory set2 = _set64(B3TypesV2.RULESET_V2);
        assertFalse(_verify(digestV2, _hash(set2), set2, proof), "V1 proof, digest V2, ruleset 2");
        assertFalse(_verify(digestV2, SET0_HASH_V1, _set64(1), proof), "V1 proof, digest V2, ruleset 1");
    }

    /// In-EVM: a signature over the V1 digest of a statement verifies only for
    /// that V1 digest, never for the statement's digest V2, and a V2 proof
    /// never passes the V1 prover.
    function test_V1AndV2DigestsNeverCrossVerify_InEvm() public view {
        Signer.TestSet memory s = Signer.makeUniformSet(harness, 3, SEED_A, 4, 250);
        B3TypesV2.FinalizedBlockV2 memory fb = _statementV2(311, FILL_01, FILL_02, FILL_03, 3);
        bytes32 digestV1 = _v1Digest(D0_DOMAIN, fb.height, fb.blockHash, fb.withdrawalRoot, fb.validatorSetHash, 3);
        bytes32 digestV2 = B3TypesV2.digest(D0_DOMAIN, fb);
        assertNotEq(digestV1, digestV2);

        bytes memory proofV1 = Signer.sign(harness, s, digestV1, Signer.ids(2));
        bytes memory proofV2 = Signer.sign(harness, s, digestV2, Signer.ids(2));

        assertTrue(_verify(digestV1, s.setHash, s.header, proofV1), "sanity: V1-digest proof is a valid signature");
        assertFalse(_verify(digestV2, s.setHash, s.header, proofV1), "V1 proof under digest V2");
        assertTrue(_verify(digestV2, s.setHash, s.header, proofV2), "V2 proof under digest V2");

        B3Types.SetHeader memory h1 = Signer.copyHeader(s.header);
        h1.rulesetVersion = 1;
        B3Types.FinalizedBlock memory v1Block =
            _v1Block(fb.height, fb.blockHash, fb.withdrawalRoot, fb.validatorSetHash, 3);
        assertTrue(proverV1.verify(D0_DOMAIN, v1Block, _hash(h1), h1, proofV1), "sanity: in-EVM signer makes V1 proofs");
        assertFalse(proverV1.verify(D0_DOMAIN, v1Block, _hash(h1), h1, proofV2), "V2 proof under the V1 prover");
    }

    // =====================================================================
    // Part 2: in-EVM signer
    // =====================================================================

    function test_InEvmScalarSanity() public view {
        assertEq(Signer.g1Mul(1), Signer.G1_GENERATOR, "1 * G");
        assertEq(Signer.g1Mul(Signer.R - 1), prover.negativeGenerator(), "(r - 1) * G = -G");
        assertEq(Signer.g1Mul(Signer.R + 1), Signer.G1_GENERATOR, "scalars reduce mod r");
        // Keys depend on the seed only (B-37 fixtures rely on it).
        Signer.TestSet memory a0 = Signer.makeUniformSet(harness, 0, SEED_A, 4, 250);
        Signer.TestSet memory a1 = Signer.makeUniformSet(harness, 1, SEED_A, 4, 250);
        Signer.TestSet memory b2 = Signer.makeUniformSet(harness, 2, SEED_B, 4, 250);
        assertEq(a0.header.aggregatePubkey, a1.header.aggregatePubkey, "same seed, same keys");
        assertEq(a0.header.membersRoot, a1.header.membersRoot, "same seed, same members");
        assertTrue(a0.setHash != a1.setHash, "epoch is in the set hash");
        for (uint256 i = 0; i < 4; ++i) {
            for (uint256 j = 0; j < 4; ++j) {
                assertTrue(keccak256(a0.members[i].pk48) != keccak256(b2.members[j].pk48), "fresh seed, no shared key");
            }
        }
    }

    function test_InEvm3of4PassesForEveryAbsentMemberAnd4of4() public view {
        Signer.TestSet memory s = Signer.makeUniformSet(harness, 0, SEED_A, 4, 250);
        assertEq(uint256(s.header.rulesetVersion), 2);
        assertEq(uint256(s.header.totalWeight), 1000);
        assertEq(uint256(s.header.quorumWeight), 667);
        bytes32 digest = _digest("3-of-4");
        for (uint32 absent = 0; absent < 4; ++absent) {
            assertTrue(_verify(digest, s.setHash, s.header, Signer.sign(harness, s, digest, Signer.ids(absent))));
        }
        assertTrue(_verify(digest, s.setHash, s.header, Signer.sign(harness, s, digest, Signer.none())), "4-of-4");
    }

    /// Headcount alone refuses 2-of-4 even when the two carry the stake quorum.
    function test_InEvm2of4FailsHeadcount() public view {
        uint64[] memory w = new uint64[](4);
        (w[0], w[1], w[2], w[3]) = (10, 10, 1, 1); // W = 22, quorum 15
        Signer.TestSet memory s = Signer.makeSet(harness, 0, SEED_A, 4, w);
        bytes32 digest = _digest("2-of-4");
        assertFalse(
            _verify(digest, s.setHash, s.header, Signer.sign(harness, s, digest, Signer.ids(2, 3))),
            "2 signers with 20/22 stake"
        );
        assertTrue(_verify(digest, s.setHash, s.header, Signer.sign(harness, s, digest, Signer.ids(3))), "3-of-4");

        Signer.TestSet memory u = Signer.makeUniformSet(harness, 0, SEED_A, 4, 250);
        assertFalse(
            _verify(digest, u.setHash, u.header, Signer.sign(harness, u, digest, Signer.ids(0, 1))), "equal weights"
        );
    }

    /// Stake alone refuses 3-of-4 when the absent member holds over a third.
    function test_InEvmUnequalWeightsFailStakeQuorum() public view {
        uint64[] memory w = new uint64[](4);
        (w[0], w[1], w[2], w[3]) = (1, 1, 1, 7); // W = 10, quorum 7
        Signer.TestSet memory s = Signer.makeSet(harness, 0, SEED_A, 4, w);
        assertEq(uint256(s.header.quorumWeight), 7);
        bytes32 digest = _digest("stake");
        assertFalse(
            _verify(digest, s.setHash, s.header, Signer.sign(harness, s, digest, Signer.ids(3))), "3 signers, 3/10"
        );
        assertTrue(
            _verify(digest, s.setHash, s.header, Signer.sign(harness, s, digest, Signer.ids(0))), "3 signers, 9/10"
        );
    }

    function test_InEvmWrongSetHashFails() public view {
        Signer.TestSet memory s = Signer.makeUniformSet(harness, 0, SEED_A, 4, 250);
        Signer.TestSet memory other = Signer.makeUniformSet(harness, 0, SEED_B, 4, 250);
        bytes32 digest = _digest("set hash");
        bytes memory proof = Signer.sign(harness, s, digest, Signer.ids(1));
        assertTrue(_verify(digest, s.setHash, s.header, proof));

        assertFalse(_verify(digest, other.setHash, s.header, proof), "another set's hash");
        assertFalse(_verify(digest, bytes32(0), s.header, proof), "zero hash");
        assertFalse(_verify(digest, other.setHash, other.header, proof), "another set (consistent hash)");

        B3Types.SetHeader memory moved = Signer.copyHeader(s.header);
        moved.epoch = 1;
        assertFalse(_verify(digest, s.setHash, moved, proof), "header altered under a fixed hash");

        B3Types.SetHeader memory v1 = Signer.copyHeader(s.header);
        v1.rulesetVersion = 1;
        assertFalse(_verify(digest, _hash(v1), v1, proof), "ruleset 1");

        B3Types.SetHeader memory badQuorum = Signer.copyHeader(s.header);
        badQuorum.quorumWeight = 666;
        assertFalse(_verify(digest, _hash(badQuorum), badQuorum, proof), "quorum not floor(2W/3)+1");
    }

    function test_InEvmNonAscendingAbsentIndicesFail() public view {
        Signer.TestSet memory s = Signer.makeUniformSet(harness, 0, SEED_A, 7, 1); // headcount quorum 5
        bytes32 digest = _digest("order");
        Signer.Parts memory p = Signer.signParts(harness, s, digest, Signer.ids(6, 5));
        assertEq(uint256(p.absent[0].index), 5, "signer sorts absentees");
        assertTrue(_verify(digest, s.setHash, s.header, Signer.encode(p)), "ascending");

        Signer.Parts memory swapped = Signer.signParts(harness, s, digest, Signer.ids(5, 6));
        (swapped.absent[0], swapped.absent[1]) = (swapped.absent[1], swapped.absent[0]);
        assertFalse(_verify(digest, s.setHash, s.header, Signer.encode(swapped)), "descending");

        Signer.Parts memory duplicate = Signer.signParts(harness, s, digest, Signer.ids(5, 6));
        duplicate.absent[1] = duplicate.absent[0];
        assertFalse(_verify(digest, s.setHash, s.header, Signer.encode(duplicate)), "duplicate");
    }

    /// Clearing signer bit 0 and setting trailing bit 4 keeps the count
    /// (3 + 1 absent = 4), and the signature and aggregate still match signers
    /// {0, 1, 2}: only the trailing-bit rule refuses this bitmap.
    function test_InEvmNonzeroTrailingBitmapBitFails() public view {
        Signer.TestSet memory s = Signer.makeUniformSet(harness, 0, SEED_A, 4, 250);
        bytes32 digest = _digest("trailing");
        Signer.Parts memory p = Signer.signParts(harness, s, digest, Signer.ids(3));
        assertEq(uint8(p.bitmap[0]), 0x07);
        assertTrue(_verify(digest, s.setHash, s.header, Signer.encode(p)));
        p.bitmap[0] = 0x16;
        assertFalse(_verify(digest, s.setHash, s.header, Signer.encode(p)), "trailing bit 4");

        // Same rule in a partial second byte (n = 12).
        Signer.TestSet memory t = Signer.makeUniformSet(harness, 0, SEED_A, 12, 1); // headcount quorum 9
        Signer.Parts memory q = Signer.signParts(harness, t, digest, Signer.ids(9, 10, 11));
        assertTrue(_verify(digest, t.setHash, t.header, Signer.encode(q)));
        q.bitmap[1] = bytes1(uint8(q.bitmap[1]) | 0x10); // bit 12
        q.bitmap[0] = bytes1(uint8(q.bitmap[0]) & 0xfe); // clear signer 0
        assertFalse(_verify(digest, t.setHash, t.header, Signer.encode(q)), "trailing bit 12");
    }

    function test_InEvmAbsentWitnessNegatives() public view {
        Signer.TestSet memory s = Signer.makeUniformSet(harness, 0, SEED_A, 4, 250);
        bytes32 digest = _digest("absent");
        Signer.Parts memory base = Signer.signParts(harness, s, digest, Signer.ids(3));
        assertTrue(_verify(digest, s.setHash, s.header, Signer.encode(base)));

        // Absent member also flagged as signer, with the count kept (bits {0,1,3});
        // the subtraction would still match signers {0,1,2}.
        Signer.Parts memory p = _copyParts(base);
        p.bitmap[0] = 0x0b;
        assertFalse(_verify(digest, s.setHash, s.header, Signer.encode(p)), "absent flagged as signer");

        p = _copyParts(base);
        p.absent[0].index = 4;
        assertFalse(_verify(digest, s.setHash, s.header, Signer.encode(p)), "index >= count");

        p = _copyParts(base);
        p.absent[0].siblings[0] = bytes32(uint256(p.absent[0].siblings[0]) ^ 1);
        assertFalse(_verify(digest, s.setHash, s.header, Signer.encode(p)), "bad member path");

        p = _copyParts(base);
        p.absent[0].pubkey = s.members[2].pk48;
        assertFalse(_verify(digest, s.setHash, s.header, Signer.encode(p)), "compressed key mismatch");

        p = _copyParts(base);
        p.absent[0].pubkey = s.members[2].pk48;
        p.absent[0].uncompressedPubkey = s.members[2].pk128;
        assertFalse(_verify(digest, s.setHash, s.header, Signer.encode(p)), "another member's key");

        p = _copyParts(base);
        p.absent[0].weight = 249;
        assertFalse(_verify(digest, s.setHash, s.header, Signer.encode(p)), "weight not committed");

        p = _copyParts(base);
        p.absent[0].uncompressedPubkey[0] = 0x01;
        assertFalse(_verify(digest, s.setHash, s.header, Signer.encode(p)), "witness padding");

        p = _copyParts(base);
        p.absent[0].pubkey = bytes.concat(s.members[3].pk48, hex"00");
        assertFalse(_verify(digest, s.setHash, s.header, Signer.encode(p)), "49-byte key");

        // Witness count must equal count - signers.
        p = _copyParts(base);
        p.absent = new BlsDigestProver.Absent[](0);
        assertFalse(_verify(digest, s.setHash, s.header, Signer.encode(p)), "missing absent witness");
    }

    function test_InEvmSignatureAndAggregateNegatives() public view {
        Signer.TestSet memory s = Signer.makeUniformSet(harness, 0, SEED_A, 4, 250);
        Signer.TestSet memory other = Signer.makeUniformSet(harness, 0, SEED_B, 4, 250);
        bytes32 digest = _digest("signature");
        Signer.Parts memory base = Signer.signParts(harness, s, digest, Signer.ids(3));

        Signer.Parts memory p = _copyParts(base);
        p.signature = Signer.aggregateSignature(harness, s, digest, Signer.ids(0, 1));
        assertFalse(_verify(digest, s.setHash, s.header, Signer.encode(p)), "fewer signers than claimed");

        p = _copyParts(base);
        p.signature = Signer.aggregateSignature(harness, s, _digest("another"), Signer.ids(0, 1, 2));
        assertFalse(_verify(digest, s.setHash, s.header, Signer.encode(p)), "signature over another digest");

        p = _copyParts(base);
        p.signature = Signer.aggregateSignature(harness, other, digest, Signer.ids(0, 1, 2));
        assertFalse(_verify(digest, s.setHash, s.header, Signer.encode(p)), "another set's signers");

        p = _copyParts(base);
        p.signature = new bytes(256);
        assertFalse(_verify(digest, s.setHash, s.header, Signer.encode(p)), "zero signature");

        p = _copyParts(base);
        p.signature = bytes.concat(base.signature, hex"00");
        assertFalse(_verify(digest, s.setHash, s.header, Signer.encode(p)), "257-byte signature");

        p = _copyParts(base);
        p.signature[255] = bytes1(uint8(p.signature[255]) ^ 0x01);
        assertFalse(_verify(digest, s.setHash, s.header, Signer.encode(p)), "signature not a G2 point");

        p = _copyParts(base);
        p.bitmap = hex"0700";
        assertFalse(_verify(digest, s.setHash, s.header, Signer.encode(p)), "bitmap length");

        p = _copyParts(base);
        p.aggregate = other.aggregate128;
        assertFalse(_verify(digest, s.setHash, s.header, Signer.encode(p)), "aggregate witness of another set");

        // -aggregate shares x with the header key; only the y flag differs.
        p = _copyParts(base);
        p.aggregate = _negateG1(s.aggregate128);
        assertFalse(_verify(digest, s.setHash, s.header, Signer.encode(p)), "negated aggregate witness");

        p = _copyParts(base);
        p.bitmap[0] = 0x00;
        p.absent = new BlsDigestProver.Absent[](4);
        for (uint32 i = 0; i < 4; ++i) {
            p.absent[i] = Signer.absentWitness(s, i);
        }
        p.signature = Signer.aggregateSignature(harness, s, digest, Signer.none());
        assertFalse(_verify(digest, s.setHash, s.header, Signer.encode(p)), "no signers");
    }

    /// Forgery with keys outside the set: the witness W = (sum of three foreign
    /// secrets + the absent member's secret) * G, minus the absent key, is
    /// exactly the foreign signers' key. Every other rule passes; only the
    /// match between W and the header's aggregate refuses it.
    function test_InEvmAggregateWitnessMustMatchHeader() public view {
        Signer.TestSet memory x = Signer.makeUniformSet(harness, 0, SEED_A, 4, 250);
        Signer.TestSet memory foreign = Signer.makeUniformSet(harness, 0, SEED_B, 4, 250);
        bytes32 digest = _digest("rogue witness");

        uint256 scalar = addmod(foreign.members[0].secret, foreign.members[1].secret, Signer.R);
        scalar = addmod(scalar, foreign.members[2].secret, Signer.R);
        scalar = addmod(scalar, x.members[3].secret, Signer.R);

        Signer.Parts memory p;
        p.bitmap = hex"07";
        p.signature = Signer.aggregateSignature(harness, foreign, digest, Signer.ids(0, 1, 2));
        p.aggregate = Signer.g1Mul(scalar);
        p.absent = new BlsDigestProver.Absent[](1);
        p.absent[0] = Signer.absentWitness(x, 3);
        assertFalse(_verify(digest, x.setHash, x.header, Signer.encode(p)), "aggregate witness not the header's");
    }

    /// All four signed, but the bitmap shows three and no absent witness is
    /// given. The pairing would pass; the count rule (signers + absent == n)
    /// refuses it.
    function test_InEvmBitmapAndAbsentMustAccountForEveryMember() public view {
        Signer.TestSet memory s = Signer.makeUniformSet(harness, 0, SEED_A, 4, 250);
        bytes32 digest = _digest("count");
        Signer.Parts memory p = Signer.signParts(harness, s, digest, Signer.none());
        assertEq(uint8(p.bitmap[0]), 0x0f);
        assertTrue(_verify(digest, s.setHash, s.header, Signer.encode(p)));
        p.bitmap[0] = 0x07;
        assertFalse(_verify(digest, s.setHash, s.header, Signer.encode(p)), "member 3 unaccounted");
    }

    /// Rogue keys (which B3's proof-of-possession rule excludes) whose signer
    /// sum is the point at infinity: with a zero signature the pairing holds
    /// for any digest, so the prover must refuse an infinity signer aggregate
    /// and a zero signature.
    function test_InEvmSignerKeysSummingToInfinityRefused() public view {
        uint256[] memory secrets = new uint256[](4);
        secrets[0] = Signer.secretKey(SEED_A, 0);
        secrets[1] = Signer.secretKey(SEED_A, 1);
        secrets[2] = Signer.R - addmod(secrets[0], secrets[1], Signer.R);
        secrets[3] = Signer.secretKey(SEED_A, 3);
        Signer.TestSet memory s = Signer.makeSetFromSecrets(harness, 0, secrets, Signer.uniformWeights(4, 250));

        bytes32 digest = _digest("infinity");
        Signer.Parts memory p = Signer.signParts(harness, s, digest, Signer.ids(3));
        assertEq(p.signature, new bytes(256), "signers' signature is the identity");
        assertFalse(_verify(digest, s.setHash, s.header, Signer.encode(p)), "infinity signer aggregate");
        assertFalse(_verify(_digest("any other"), s.setHash, s.header, Signer.encode(p)), "for any digest");
    }

    /// The bridge-sized sets of the gas plan at their minimum headcount quorum.
    function test_InEvmLargerSetsAtMinimumQuorum() public view {
        bytes32 digest = _digest("sizes");
        uint32[2] memory sizes = [uint32(34), uint32(64)];
        for (uint256 k = 0; k < sizes.length; ++k) {
            uint32 n = sizes[k];
            Signer.TestSet memory s = Signer.makeUniformSet(harness, 0, SEED_B, n, 100);
            uint32[] memory absent = Signer.minQuorumAbsent(n);
            assertEq(absent.length, n == 34 ? 11 : 21);
            bytes memory proof = Signer.sign(harness, s, digest, absent);
            uint256 gasBefore = gasleft();
            bool ok = prover.verifyDigest{gas: VERIFY_GAS}(digest, s.setHash, s.header, proof);
            uint256 used = gasBefore - gasleft();
            assertTrue(ok, "minimum quorum");
            ConsoleV2.log(
                n == 34 ? "verifyDigest gas, 34 members, 11 absent" : "verifyDigest gas, 64 members, 21 absent", used
            );

            uint32[] memory oneMore = Signer.range(uint32((uint256(n) * 2) / 3), n);
            assertFalse(_verify(digest, s.setHash, s.header, Signer.sign(harness, s, digest, oneMore)), "one short");
        }
    }

    /// A shared-vector statement (finalized_block_v2[2]: mainnet domain, flags 1),
    /// signed in-EVM over its digest V2.
    function test_InEvmSignsSharedVectorDigestV2() public view {
        string memory j = readVectorsV2();
        string memory p = ".finalized_block_v2[2]";
        B3TypesV2.FinalizedBlockV2 memory fb = B3TypesV2.FinalizedBlockV2({
            lineageId: vm.parseJsonBytes32(j, string.concat(p, ".lineage_id")),
            height: uint64(vm.parseJsonUint(j, string.concat(p, ".height"))),
            blockHash: vm.parseJsonBytes32(j, string.concat(p, ".block_hash")),
            withdrawalRoot: vm.parseJsonBytes32(j, string.concat(p, ".withdrawal_root")),
            validatorSetHash: vm.parseJsonBytes32(j, string.concat(p, ".validator_set_hash")),
            epoch: uint64(vm.parseJsonUint(j, string.concat(p, ".epoch"))),
            checkpointIndex: uint64(vm.parseJsonUint(j, string.concat(p, ".checkpoint_index"))),
            checkpointHistoryRoot: vm.parseJsonBytes32(j, string.concat(p, ".checkpoint_history_root")),
            bridgeFlags: uint8(vm.parseJsonUint(j, string.concat(p, ".bridge_flags")))
        });
        bytes32 domain = vm.parseJsonBytes32(j, string.concat(p, ".chain_domain"));
        bytes32 digest = B3TypesV2.digest(domain, fb);
        assertEq(digest, vm.parseJsonBytes32(j, string.concat(p, ".digest_v2")), "shared digest V2");
        assertEq(uint256(fb.bridgeFlags), 1);

        Signer.TestSet memory s = Signer.makeUniformSet(harness, fb.epoch, SEED_A, 4, 250);
        bytes memory proof = Signer.sign(harness, s, digest, Signer.ids(0));
        assertTrue(_verify(digest, s.setHash, s.header, proof), "signed digest V2");
        fb.bridgeFlags = 0;
        assertFalse(_verify(B3TypesV2.digest(domain, fb), s.setHash, s.header, proof), "flags are signed");
    }

    // =====================================================================
    // Helpers
    // =====================================================================

    function _verify(bytes32 digest, bytes32 setHash, B3Types.SetHeader memory set, bytes memory proof)
        private
        view
        returns (bool)
    {
        return prover.verifyDigest{gas: VERIFY_GAS}(digest, setHash, set, proof);
    }

    function _hash(B3Types.SetHeader memory h) private pure returns (bytes32) {
        return B3Types.hashSetHeader(h);
    }

    function _digest(string memory label) private pure returns (bytes32) {
        return keccak256(abi.encodePacked("ProverV2Test/digest/", label));
    }

    function _flip(bytes32 value) private pure returns (bytes32) {
        return value ^ bytes32(uint256(1));
    }

    /// V1 FinalityDigest (contracts/src/BlsCertificateProver.sol:158-176), recomputed here.
    function _v1Digest(
        bytes32 domain,
        uint64 height,
        bytes32 blockHash,
        bytes32 withdrawalRoot,
        bytes32 validatorSetHash,
        uint64 epoch
    ) private pure returns (bytes32) {
        bytes32 tagHash = sha256("B3/FINALITY/V1");
        return
            sha256(
                abi.encodePacked(tagHash, tagHash, domain, height, blockHash, withdrawalRoot, validatorSetHash, epoch)
            );
    }

    function _v1Block(uint64 height, bytes32 blockHash, bytes32 withdrawalRoot, bytes32 validatorSetHash, uint64 epoch)
        private
        pure
        returns (B3Types.FinalizedBlock memory b)
    {
        b = B3Types.FinalizedBlock({
            height: height,
            blockHash: blockHash,
            withdrawalRoot: withdrawalRoot,
            validatorSetHash: validatorSetHash,
            epoch: epoch
        });
    }

    function _statementV2(uint64 height, bytes32 blockHash, bytes32 withdrawalRoot, bytes32 successor, uint64 epoch)
        private
        pure
        returns (B3TypesV2.FinalizedBlockV2 memory fb)
    {
        fb.lineageId = keccak256("ProverV2Test/lineage");
        fb.height = height;
        fb.blockHash = blockHash;
        fb.withdrawalRoot = withdrawalRoot;
        fb.validatorSetHash = successor;
        fb.epoch = epoch;
        fb.checkpointIndex = 1;
        fb.checkpointHistoryRoot = keccak256("ProverV2Test/history");
        fb.bridgeFlags = 0;
    }

    function _oneMemberSet(uint16 ruleset) private pure returns (B3Types.SetHeader memory h) {
        h.epoch = 7;
        h.rulesetVersion = ruleset;
        h.validatorCount = 1;
        h.totalWeight = 333;
        h.quorumWeight = 223;
        h.aggregatePubkey = ONE_COMPRESSED_PUBLIC_KEY;
        bytes32 node = keccak256(abi.encodePacked(uint32(0), ONE_COMPRESSED_PUBLIC_KEY, uint64(333)));
        bytes32 zero;
        for (uint256 level = 0; level < 13; ++level) {
            node = keccak256(abi.encodePacked(node, zero));
            zero = keccak256(abi.encodePacked(zero, zero));
        }
        h.membersRoot = node;
    }

    function _set64(uint16 ruleset) private pure returns (B3Types.SetHeader memory h) {
        h.epoch = 0;
        h.rulesetVersion = ruleset;
        h.validatorCount = 64;
        h.totalWeight = 64;
        h.quorumWeight = 43;
        h.aggregatePubkey = SET64_AGGREGATE_PUBLIC_KEY;
        h.membersRoot = SET64_MEMBERS_ROOT;
    }

    function _bootstrapSet(uint16 ruleset) private pure returns (B3Types.SetHeader memory h) {
        h.epoch = 0;
        h.rulesetVersion = ruleset;
        h.validatorCount = 4;
        h.totalWeight = 4;
        h.quorumWeight = 3;
        h.aggregatePubkey = BOOTSTRAP_AGGREGATE_PUBLIC_KEY;
        h.membersRoot = BOOTSTRAP_MEMBERS_ROOT;
    }

    function _noAbsent() private pure returns (BlsDigestProver.Absent[] memory) {
        return new BlsDigestProver.Absent[](0);
    }

    function _decode(bytes memory proof) private pure returns (Signer.Parts memory p) {
        (p.bitmap, p.signature, p.aggregate, p.absent) =
            abi.decode(proof, (bytes, bytes, bytes, BlsDigestProver.Absent[]));
    }

    /// Deep copy through the ABI, so each negative case starts clean.
    function _copyParts(Signer.Parts memory p) private pure returns (Signer.Parts memory) {
        return _decode(Signer.encode(p));
    }

    function _dropFirst(BlsDigestProver.Absent[] memory a) private pure returns (BlsDigestProver.Absent[] memory out) {
        out = new BlsDigestProver.Absent[](a.length - 1);
        for (uint256 i = 1; i < a.length; ++i) {
            out[i - 1] = a[i];
        }
    }

    /// -P for a 128-byte EIP-2537 G1 point: same x, y' = p - y.
    function _negateG1(bytes memory point) private pure returns (bytes memory out) {
        uint256 pHi = 0x1a0111ea397fe69a4b1ba7b6434bacd7;
        uint256 pLo = 0x64774b84f38512bf6730d2a0f6b0f6241eabfffeb153ffffb9feffffffffaaab;
        uint256 yHi;
        uint256 yLo;
        assembly ("memory-safe") {
            yHi := mload(add(point, 0x60)) // bytes [64, 96): 16 zero bytes || high 16 bytes of y
            yLo := mload(add(point, 0x80)) // bytes [96, 128): low 32 bytes of y
        }
        uint256 borrow = yLo > pLo ? 1 : 0;
        uint256 lo;
        unchecked {
            lo = pLo - yLo;
        }
        uint256 hi = pHi - yHi - borrow;
        out = bytes.concat(point);
        assembly ("memory-safe") {
            mstore(add(out, 0x60), hi)
            mstore(add(out, 0x80), lo)
        }
    }
}
