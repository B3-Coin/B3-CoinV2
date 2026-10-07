// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {B3Types} from "../../../src/IB3FinalityProver.sol";
import {B3TypesV2} from "../../../src/v2/B3TypesV2.sol";
import {BlsDigestProver} from "../../../src/v2/BlsDigestProver.sol";
import {MerkleTreeV2} from "./MerkleTreeV2.sol";

/// Test-only: the production prover plus external entries to its own
/// hash-to-G2 and point helpers, so the in-EVM signer below hashes messages
/// with exactly the code that verifies them. verifyDigest is inherited
/// unchanged; tests still verify against a plain BlsDigestProver deployment.
contract BlsDigestProverHarnessV2 is BlsDigestProver {
    /// H(digest) on G2 under the POP DST, 256-byte EIP-2537 form.
    function hashToG2(bytes32 digest) external view returns (bytes memory) {
        return _hashToG2(abi.encodePacked(digest));
    }

    /// IETF compressed form of a 128-byte EIP-2537 G1 point: the 48 x bytes
    /// with 0x80 set, plus 0x20 when y > p - y. Not for the point at infinity.
    function compressG1(bytes memory uncompressed) external pure returns (bytes memory out) {
        require(uncompressed.length == 128, "compressG1: length");
        for (uint256 i = 0; i < 16; ++i) {
            require(uncompressed[i] == 0 && uncompressed[64 + i] == 0, "compressG1: padding");
        }
        out = _slice(uncompressed, 16, 48);
        // x < p < 2^381, so the three flag bits of the top byte are free.
        require(uint8(out[0]) & 0xe0 == 0, "compressG1: x");
        bytes memory y = _slice(uncompressed, 80, 48);
        bytes memory pMinusY = _slice(_subFieldPadded(_pad64(y)), 16, 48);
        uint8 flags = _greaterThan48(y, pMinusY) ? 0xa0 : 0x80;
        out[0] = bytes1(uint8(out[0]) | flags);
    }

    function compressedG1Matches(bytes memory uncompressed, bytes memory compressed) external pure returns (bool) {
        return _compressedG1Matches(uncompressed, compressed);
    }

    function memberRoot(uint32 index, bytes memory pubkey, uint64 weight, bytes32[13] memory siblings)
        external
        pure
        returns (bytes32)
    {
        return _memberRoot(index, pubkey, weight, siblings);
    }
}

/// Deterministic in-EVM BLS12-381 signer for the bridge V2 suites (test
/// only; no secret here protects anything). It derives member secrets from a
/// seed, builds a ruleset-2 SetHeader exactly as B3 commits one (depth-13
/// ordered member tree, zero padding; src/modern/withdrawal_tree.h), and
/// produces BlsDigestProver witnesses with the EIP-2537 MSM precompiles:
///
///   sk_i      = keccak256(abi.encode(seed, i)) mod (r - 1) + 1
///   pk128_i   = G1MSM(G, sk_i)                 (EIP-2537 uncompressed)
///   aggregate = G1MSM(G, sum sk_i mod r)       (header key, compressed)
///   signature = G2MSM(H(digest), sum of signer sk_i mod r)
///
/// Keys depend on the seed only, so two epochs built from one seed share
/// keys and fresh seeds give sets that share none (B-37 fixtures). The
/// cross-language check that these primitives agree with blst is the
/// committed C++ vectors in test/v2/ProverV2.t.sol.
library BlsTestSignerV2 {
    /// BLS12-381 subgroup order r.
    uint256 internal constant R = 0x73eda753299d7d483339d80809a1d80553bda402fffe5bfeffffffff00000001;
    uint256 internal constant MEMBER_DEPTH = 13;

    address private constant G1MSM = address(0x0c);
    address private constant G2MSM = address(0x0e);

    /// EIP-2537 uncompressed G1 generator: 16-byte pad || X || 16-byte pad || Y.
    bytes internal constant G1_GENERATOR =
        hex"0000000000000000000000000000000017f1d3a73197d7942695638c4fa9ac0fc3688c4f9774b905a14e3a3f171bac586c55e83ff97a1aeffb3af00adb22c6bb0000000000000000000000000000000008b3f481e3aaa0f1a09e30ed741d8ae4fcf5e095d5d00af600db18cb2c04b3edd03cc744a2888ae40caa232946c5e7e1";

    struct Member {
        uint32 index;
        uint256 secret;
        bytes pk48; // canonical compressed G1
        bytes pk128; // EIP-2537 uncompressed G1
        uint64 weight;
    }

    struct TestSet {
        B3Types.SetHeader header; // ruleset 2
        bytes32 setHash;
        bytes aggregate128; // uncompressed witness of header.aggregatePubkey
        Member[] members;
        bytes32[] leaves; // member leaves, index order
    }

    /// Unpacked witness, for tests that tamper with one part.
    struct Parts {
        bytes bitmap;
        bytes signature;
        bytes aggregate;
        BlsDigestProver.Absent[] absent;
    }

    // ---------------------------------------------------------------- keys

    function secretKey(bytes32 seed, uint256 index) internal pure returns (uint256) {
        return uint256(keccak256(abi.encode(seed, index))) % (R - 1) + 1;
    }

    /// scalar * G on G1 (G1MSM with one pair).
    function g1Mul(uint256 scalar) internal view returns (bytes memory out) {
        bool ok;
        (ok, out) = G1MSM.staticcall(abi.encodePacked(G1_GENERATOR, scalar));
        require(ok && out.length == 128, "G1MSM");
    }

    /// scalar * point on G2 (G2MSM with one pair).
    function g2Mul(bytes memory point, uint256 scalar) internal view returns (bytes memory out) {
        require(point.length == 256, "G2MSM: point");
        bool ok;
        (ok, out) = G2MSM.staticcall(abi.encodePacked(point, scalar));
        require(ok && out.length == 256, "G2MSM");
    }

    // ---------------------------------------------------------------- sets

    function uniformWeights(uint256 n, uint64 weight) internal pure returns (uint64[] memory weights) {
        weights = new uint64[](n);
        for (uint256 i = 0; i < n; ++i) {
            weights[i] = weight;
        }
    }

    /// Ruleset-2 SetHeader over n members with the given weights; quorum is
    /// floor(2W/3)+1 as B3 derives it.
    function makeSet(BlsDigestProverHarnessV2 h, uint64 epoch, bytes32 seed, uint32 n, uint64[] memory weights)
        internal
        view
        returns (TestSet memory)
    {
        uint256[] memory secrets = new uint256[](n);
        for (uint256 i = 0; i < n; ++i) {
            secrets[i] = secretKey(seed, i);
        }
        return makeSetFromSecrets(h, epoch, secrets, weights);
    }

    /// As makeSet with explicit secrets (each in 1 .. r-1), for crafted key
    /// sets such as signers whose keys sum to the point at infinity. The
    /// whole set's aggregate must not be infinity.
    function makeSetFromSecrets(
        BlsDigestProverHarnessV2 h,
        uint64 epoch,
        uint256[] memory secrets,
        uint64[] memory weights
    ) internal view returns (TestSet memory s) {
        uint256 n = secrets.length;
        require(n >= 1 && n <= B3Types.MAX_VALIDATORS && weights.length == n, "makeSet: shape");
        s.members = new Member[](n);
        s.leaves = new bytes32[](n);
        uint256 totalWeight;
        uint256 aggregateSecret;
        for (uint32 i = 0; i < n; ++i) {
            uint256 secret = secrets[i];
            require(secret != 0 && secret < R, "makeSet: secret");
            bytes memory pk128 = g1Mul(secret);
            bytes memory pk48 = h.compressG1(pk128);
            s.members[i] = Member({index: i, secret: secret, pk48: pk48, pk128: pk128, weight: weights[i]});
            s.leaves[i] = MerkleTreeV2.memberLeaf(i, pk48, weights[i]);
            totalWeight += weights[i];
            aggregateSecret = addmod(aggregateSecret, secret, R);
        }
        require(totalWeight != 0 && totalWeight <= type(uint64).max, "makeSet: weight");
        require(aggregateSecret != 0, "makeSet: aggregate at infinity");

        s.aggregate128 = g1Mul(aggregateSecret);
        s.header = B3Types.SetHeader({
            epoch: epoch,
            rulesetVersion: B3TypesV2.RULESET_V2,
            // forge-lint: disable-next-line(unsafe-typecast)
            validatorCount: uint32(n),
            // forge-lint: disable-next-line(unsafe-typecast)
            totalWeight: uint64(totalWeight),
            // forge-lint: disable-next-line(unsafe-typecast)
            quorumWeight: uint64(B3Types.quorumWeight(uint64(totalWeight))),
            aggregatePubkey: h.compressG1(s.aggregate128),
            membersRoot: MerkleTreeV2.root(s.leaves, n, MEMBER_DEPTH)
        });
        s.setHash = B3Types.hashSetHeader(s.header);
    }

    function makeUniformSet(BlsDigestProverHarnessV2 h, uint64 epoch, bytes32 seed, uint32 n, uint64 weight)
        internal
        view
        returns (TestSet memory)
    {
        return makeSet(h, epoch, seed, n, uniformWeights(n, weight));
    }

    /// Deep copy, so a test can alter a header without aliasing the set's.
    function copyHeader(B3Types.SetHeader memory header) internal pure returns (B3Types.SetHeader memory out) {
        out = B3Types.SetHeader({
            epoch: header.epoch,
            rulesetVersion: header.rulesetVersion,
            validatorCount: header.validatorCount,
            totalWeight: header.totalWeight,
            quorumWeight: header.quorumWeight,
            aggregatePubkey: bytes.concat(header.aggregatePubkey),
            membersRoot: header.membersRoot
        });
    }

    // ------------------------------------------------------------- signing

    /// Proof for `digest` signed by every member not listed in `absentIdx`
    /// (any order, no duplicates); absent witnesses are ascending.
    function sign(BlsDigestProverHarnessV2 h, TestSet memory s, bytes32 digest, uint32[] memory absentIdx)
        internal
        view
        returns (bytes memory)
    {
        return encode(signParts(h, s, digest, absentIdx));
    }

    function signParts(BlsDigestProverHarnessV2 h, TestSet memory s, bytes32 digest, uint32[] memory absentIdx)
        internal
        view
        returns (Parts memory p)
    {
        uint32 n = s.header.validatorCount;
        uint32[] memory absentSorted = _sortedUnique(absentIdx, n);
        bool[] memory isAbsent = new bool[](n);
        for (uint256 k = 0; k < absentSorted.length; ++k) {
            isAbsent[absentSorted[k]] = true;
        }

        uint32[] memory signers = new uint32[](n - absentSorted.length);
        uint256 cursor;
        for (uint32 i = 0; i < n; ++i) {
            if (!isAbsent[i]) signers[cursor++] = i;
        }

        p.bitmap = bitmapOf(n, signers);
        p.signature = aggregateSignature(h, s, digest, signers);
        p.aggregate = s.aggregate128;
        p.absent = new BlsDigestProver.Absent[](absentSorted.length);
        for (uint256 k = 0; k < absentSorted.length; ++k) {
            p.absent[k] = absentWitness(s, absentSorted[k]);
        }
    }

    function encode(Parts memory p) internal pure returns (bytes memory) {
        return abi.encode(p.bitmap, p.signature, p.aggregate, p.absent);
    }

    /// G2MSM(H(digest), sum of the listed members' secrets): the aggregate
    /// signature of exactly those members, whatever the bitmap claims.
    function aggregateSignature(BlsDigestProverHarnessV2 h, TestSet memory s, bytes32 digest, uint32[] memory signerIdx)
        internal
        view
        returns (bytes memory)
    {
        uint256 secret;
        for (uint256 k = 0; k < signerIdx.length; ++k) {
            secret = addmod(secret, s.members[signerIdx[k]].secret, R);
        }
        return g2Mul(h.hashToG2(digest), secret);
    }

    /// LSB-first signer bitmap of ceil(n/8) bytes.
    function bitmapOf(uint32 n, uint32[] memory signerIdx) internal pure returns (bytes memory bitmap) {
        bitmap = new bytes((uint256(n) + 7) / 8);
        for (uint256 k = 0; k < signerIdx.length; ++k) {
            uint32 i = signerIdx[k];
            require(i < n, "bitmap: index");
            // forge-lint: disable-next-line(unsafe-typecast)
            uint8 bit = uint8(1) << uint8(i & 7); // i & 7 < 8
            bitmap[i >> 3] = bytes1(uint8(bitmap[i >> 3]) | bit);
        }
    }

    /// Absent-member witness with its ordered depth-13 path.
    function absentWitness(TestSet memory s, uint32 index) internal pure returns (BlsDigestProver.Absent memory a) {
        Member memory m = s.members[index];
        a.index = index;
        a.pubkey = m.pk48;
        a.weight = m.weight;
        a.uncompressedPubkey = m.pk128;
        a.siblings = MerkleTreeV2.path13(s.leaves, s.members.length, index);
    }

    // ------------------------------------------------------- index helpers

    function none() internal pure returns (uint32[] memory) {
        return new uint32[](0);
    }

    function ids(uint32 a) internal pure returns (uint32[] memory out) {
        out = new uint32[](1);
        out[0] = a;
    }

    function ids(uint32 a, uint32 b) internal pure returns (uint32[] memory out) {
        out = new uint32[](2);
        out[0] = a;
        out[1] = b;
    }

    function ids(uint32 a, uint32 b, uint32 c) internal pure returns (uint32[] memory out) {
        out = new uint32[](3);
        out[0] = a;
        out[1] = b;
        out[2] = c;
    }

    /// [from, to).
    function range(uint32 from, uint32 to) internal pure returns (uint32[] memory out) {
        require(from <= to, "range");
        out = new uint32[](to - from);
        for (uint32 i = from; i < to; ++i) {
            out[i - from] = i;
        }
    }

    /// The minimum B3 headcount quorum's absentees: the highest n - (floor(2n/3)+1) indices.
    function minQuorumAbsent(uint32 n) internal pure returns (uint32[] memory) {
        return range(uint32((uint256(n) * 2) / 3 + 1), n);
    }

    function _sortedUnique(uint32[] memory values, uint32 n) private pure returns (uint32[] memory out) {
        out = new uint32[](values.length);
        for (uint256 i = 0; i < values.length; ++i) {
            uint32 v = values[i];
            require(v < n, "absent: index");
            uint256 j = i;
            while (j != 0 && out[j - 1] > v) {
                out[j] = out[j - 1];
                --j;
            }
            require(j == 0 || out[j - 1] != v, "absent: duplicate");
            out[j] = v;
        }
    }
}
