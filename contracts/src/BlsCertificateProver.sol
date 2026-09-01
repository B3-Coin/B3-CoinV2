// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {B3Types, IB3FinalityProver} from "./IB3FinalityProver.sol";

/// @title BlsCertificateProver — the v1 §5.3 pairing prover (EIP-2537).
///
/// ┌──────────────────────────────────────────────────────────────────────┐
/// │  UNAUDITED, VECTOR-UNVALIDATED. DO NOT DEPLOY until every §8 test      │
/// │  vector (IETF/Ethereum BLS suites, bitmap edge cases, quorum          │
/// │  boundary, weight multiproofs) passes against these functions ON A    │
/// │  PECTRA (EIP-2537-enabled) FORK. The hash-to-curve, point-compression │
/// │  binding, and pairing plumbing below are the external-audit           │
/// │  centrepiece of the bridge — treat them as unverified until then.     │
/// └──────────────────────────────────────────────────────────────────────┘
///
/// Implements doc/design/b3-cross-chain-finality-v1.md §5.3 A–F: recover the
/// signed aggregate public key by SUBTRACTING absentees from the committed
/// aggregate (proving membership+weight of each absentee against members_root),
/// enforce quorum by weight, hash the FinalizedBlock to G2, and verify one
/// pairing. No validator list is stored or learned; only absentees appear.
contract BlsCertificateProver is IB3FinalityProver {
    // EIP-2537 precompiles (Pectra).
    address constant G1ADD  = address(0x0b);
    address constant G2ADD  = address(0x0d);
    address constant PAIRING = address(0x0f);
    address constant MAP_FP2_TO_G2 = address(0x11);
    address constant MODEXP = address(0x05);

    // BLS12-381 base field prime p, big-endian 48 bytes.
    bytes constant P =
        hex"1a0111ea397fe69a4b1ba7b6434bacd764774b84f38512bf6730d2a0f6b0f6241eabfffeb153ffffb9feffffffffaaab";
    // Uncompressed -(G1 generator) = (G1x, p - G1y), 128-byte EIP-2537 form.
    // Placeholder constant name; filled from the canonical generator during
    // vector validation. Pairing uses e(aggPk,Hm)·e(-G,sig)==1.
    bytes constant NEG_G1_GENERATOR = hex"";  // TODO(vector-validation): pin canonical bytes

    // hash-to-curve DST = the ciphersuite id (spec §1).
    bytes constant DST = "BLS_SIG_BLS12381G2_XMD:SHA-256_SSWU_RO_POP_";

    struct Absent { uint32 index; bytes pubkey; /*48*/ uint64 weight; }

    function verify(
        bytes32 chainDomain,
        B3Types.FinalizedBlock calldata fb,
        bytes32 /*setHash*/,
        B3Types.SetHeader calldata set,
        bytes calldata proof
    ) external view returns (bool) {
        (
            bytes memory bitmap,
            bytes memory sigUncompressed,      // 256-byte G2 witness (pairing binds it)
            bytes memory aggUncompressed,      // 128-byte G1 witness for set.aggregatePubkey
            Absent[] memory absent,
            bytes32[] memory multiproof,
            bool[] memory flags,
            bytes[] memory absentUncompressed  // 128-byte G1 per absentee
        ) = abi.decode(proof, (bytes, bytes, bytes, Absent[], bytes32[], bool[], bytes[]));

        uint32 n = set.validatorCount;

        // A — SIGNER BITMAP ≡ complement(absent).
        if (bitmap.length != (uint256(n) + 7) / 8) return false;
        if (n % 8 != 0 && (uint8(bitmap[n / 8]) >> (n % 8)) != 0) return false;   // high bits zero
        if (absent.length != absentUncompressed.length) return false;
        {
            int256 prev = -1;
            uint256 pc;
            for (uint256 i = 0; i < absent.length; i++) {
                uint32 ix = absent[i].index;
                if (int256(uint256(ix)) <= prev) return false;                    // strictly increasing
                if (ix >= n) return false;
                if ((uint8(bitmap[ix >> 3]) >> (ix & 7)) & 1 != 0) return false;   // absent ⇒ bit 0
                prev = int256(uint256(ix));
            }
            for (uint256 b = 0; b < bitmap.length; b++) pc += _popcount(uint8(bitmap[b]));
            if (pc + absent.length != n) return false;                            // bitmap ≡ complement(absent)
        }

        // B — membership+weight of each absentee via members_root multiproof.
        {
            bytes32[] memory leaves = new bytes32[](absent.length);
            for (uint256 i = 0; i < absent.length; i++) {
                if (absent[i].pubkey.length != 48) return false;
                if (!_compressEq(absentUncompressed[i], absent[i].pubkey)) return false;  // bind uncompressed↔committed
                leaves[i] = keccak256(abi.encodePacked(absent[i].index, absent[i].pubkey, absent[i].weight));
            }
            if (!_multiProofVerify(multiproof, flags, set.membersRoot, leaves)) return false;
        }

        // C — QUORUM by weight.
        uint256 absentWeight;
        for (uint256 i = 0; i < absent.length; i++) absentWeight += absent[i].weight;
        if (absentWeight > set.totalWeight) return false;
        if (set.totalWeight - absentWeight < set.quorumWeight) return false;

        // D — signed aggregate = committed aggregate − Σ absent (G1 subtraction).
        if (!_compressEq(aggUncompressed, set.aggregatePubkey)) return false;     // bind uncompressed↔committed
        bytes memory aggPk = aggUncompressed;                                     // 128-byte G1
        for (uint256 i = 0; i < absent.length; i++) {
            aggPk = _g1Add(aggPk, _g1Neg(absentUncompressed[i]));
        }
        if (_isZeroG1(aggPk)) return false;                                       // aggPk != INFINITY

        // E — message hashed to G2.
        bytes32 digest = _finalityDigest(chainDomain, fb);
        bytes memory Hm = _hashToG2(abi.encodePacked(digest));

        // F — one pairing: e(aggPk, Hm) · e(-G, sig) == 1.
        return _pairingTwo(aggPk, Hm, NEG_G1_GENERATOR, sigUncompressed);
    }

    // --- digest (spec §3) -------------------------------------------------
    function _finalityDigest(bytes32 domain, B3Types.FinalizedBlock calldata fb)
        internal pure returns (bytes32)
    {
        bytes memory tag = "B3/FINALITY/V1";
        bytes32 th = sha256(tag);
        bytes memory enc = abi.encodePacked(
            fb.height, fb.blockHash, fb.withdrawalRoot, fb.validatorSetHash, fb.epoch); // 112 B
        return sha256(abi.encodePacked(th, th, domain, enc));
    }

    // --- EIP-2537 wrappers ------------------------------------------------
    function _g1Add(bytes memory a, bytes memory b) internal view returns (bytes memory out) {
        out = new bytes(128);
        (bool ok, bytes memory r) = G1ADD.staticcall(abi.encodePacked(a, b));
        require(ok && r.length == 128, "g1add");
        out = r;
    }
    function _g1Neg(bytes memory pt) internal view returns (bytes memory) {
        // (X, Y) -> (X, p - Y): X is bytes[0:64], Y is bytes[64:128].
        bytes memory y = _slice(pt, 64, 64);
        bytes memory ny = _subFieldPadded(y);          // 64-byte padded (p - Y)
        bytes memory out = new bytes(128);
        for (uint256 i = 0; i < 64; i++) out[i] = pt[i];
        for (uint256 i = 0; i < 64; i++) out[64 + i] = ny[i];
        return out;
    }
    function _pairingTwo(bytes memory p1, bytes memory q1, bytes memory p2, bytes memory q2)
        internal view returns (bool)
    {
        (bool ok, bytes memory r) = PAIRING.staticcall(abi.encodePacked(p1, q1, p2, q2));
        return ok && r.length == 32 && r[31] == 0x01;
    }
    function _hashToG2(bytes memory msg_) internal view returns (bytes memory) {
        // expand_message_xmd(SHA256) -> 4 Fp elements -> 2× MAP_FP2_TO_G2 -> G2ADD.
        bytes memory u = _expandMessageXmd(msg_, 256);
        bytes memory q0 = _mapFp2(_fp(u, 0), _fp(u, 1));
        bytes memory q1 = _mapFp2(_fp(u, 2), _fp(u, 3));
        (bool ok, bytes memory r) = G2ADD.staticcall(abi.encodePacked(q0, q1));
        require(ok && r.length == 256, "g2add");
        return r;
    }
    function _mapFp2(bytes memory c0, bytes memory c1) internal view returns (bytes memory) {
        (bool ok, bytes memory r) = MAP_FP2_TO_G2.staticcall(abi.encodePacked(c0, c1));
        require(ok && r.length == 256, "map");
        return r;
    }

    // --- field / bytes helpers (audit-critical) ---------------------------
    /// One 64-byte EIP-2537-padded Fp element, reduced mod p from the k-th
    /// 64-byte chunk of the expand_message_xmd output (via modexp exp=1).
    function _fp(bytes memory xmd, uint256 k) internal view returns (bytes memory) {
        bytes memory chunk = _slice(xmd, k * 64, 64);
        // reduced = chunk mod p, using modexp(base=chunk, exp=1, mod=p)
        bytes memory input = abi.encodePacked(uint256(64), uint256(1), uint256(48), chunk, bytes1(0x01), P);
        (bool ok, bytes memory r) = MODEXP.staticcall(input);
        require(ok && r.length == 48, "modexp");
        bytes memory padded = new bytes(64);
        for (uint256 i = 0; i < 48; i++) padded[16 + i] = r[i];    // 16-byte zero pad
        return padded;
    }
    function _subFieldPadded(bytes memory yPadded64) internal pure returns (bytes memory) {
        // returns 64-byte padded (p - y); y is the low 48 bytes of yPadded64.
        bytes memory y = _slice(yPadded64, 16, 48);
        bytes memory pB = P;
        bytes memory diff = new bytes(48);
        uint256 borrow;
        for (uint256 i = 48; i > 0; i--) {
            int256 d = int256(uint256(uint8(pB[i - 1]))) - int256(uint256(uint8(y[i - 1]))) - int256(borrow);
            if (d < 0) { d += 256; borrow = 1; } else { borrow = 0; }
            diff[i - 1] = bytes1(uint8(uint256(d)));
        }
        bytes memory out = new bytes(64);
        for (uint256 i = 0; i < 48; i++) out[16 + i] = diff[i];
        return out;
    }
    /// Bind an uncompressed 128-byte G1 to its canonical 48-byte compressed
    /// form: same X, compression flag set, infinity flag, and sign of Y
    /// (ZCash rule: bit set iff Y is the lexicographically larger root).
    function _compressEq(bytes memory unc, bytes memory comp48) internal pure returns (bool) {
        if (unc.length != 128 || comp48.length != 48) return false;
        bytes memory x = _slice(unc, 16, 48);      // strip 16-byte pad of X
        bytes memory y = _slice(unc, 80, 48);       // strip 16-byte pad of Y
        // X bytes must match (ignoring the top 3 flag bits of comp[0]).
        if ((uint8(comp48[0]) & 0x1f) != (uint8(x[0]) & 0x1f)) return false;
        for (uint256 i = 1; i < 48; i++) if (comp48[i] != x[i]) return false;
        // flags: compression bit must be set, infinity bit must be clear.
        uint8 flags = uint8(comp48[0]) & 0xe0;
        if (flags & 0x80 == 0) return false;        // must be compressed
        if (flags & 0x40 != 0) return false;        // not infinity (checked elsewhere)
        // sign bit = Y > (p - Y).
        bool signHi = (flags & 0x20) != 0;
        bytes memory pMinusY = _slice(_subFieldPadded(_pad64(y)), 16, 48);
        bool yLarger = _gt48(y, pMinusY);
        return signHi == yLarger;
    }
    function _isZeroG1(bytes memory pt) internal pure returns (bool) {
        for (uint256 i = 0; i < pt.length; i++) if (pt[i] != 0) return false;
        return true;
    }

    // --- expand_message_xmd(SHA-256) (RFC 9380) ---------------------------
    function _expandMessageXmd(bytes memory msg_, uint256 lenInBytes) internal pure returns (bytes memory) {
        uint256 ell = (lenInBytes + 31) / 32;        // ceil / SHA256 output
        require(ell <= 255, "ell");
        bytes memory dst = DST;
        bytes1 dstLen = bytes1(uint8(dst.length));
        bytes memory zPad = new bytes(64);           // r_in_bytes = 64
        bytes32 b0 = sha256(abi.encodePacked(zPad, msg_, bytes2(uint16(lenInBytes)), bytes1(0x00), dst, dstLen));
        bytes32 b1 = sha256(abi.encodePacked(b0, bytes1(0x01), dst, dstLen));
        bytes memory out = new bytes(lenInBytes);
        _copy32(out, 0, b1);
        bytes32 prev = b1;
        for (uint256 i = 2; i <= ell; i++) {
            bytes32 bi = sha256(abi.encodePacked(b0 ^ prev, bytes1(uint8(i)), dst, dstLen));
            uint256 off = (i - 1) * 32;
            if (off < lenInBytes) _copy32Bounded(out, off, bi, lenInBytes - off);
            prev = bi;
        }
        return out;
    }

    // --- OpenZeppelin multiProofVerify semantics (inlined) ----------------
    function _multiProofVerify(bytes32[] memory proof, bool[] memory flags, bytes32 root, bytes32[] memory leaves)
        internal pure returns (bool)
    {
        uint256 leavesLen = leaves.length;
        uint256 totalHashes = flags.length;
        if (leavesLen + proof.length != totalHashes + 1) return false;
        bytes32[] memory hashes = new bytes32[](totalHashes);
        uint256 leafPos; uint256 hashPos; uint256 proofPos;
        for (uint256 i = 0; i < totalHashes; i++) {
            bytes32 a = leafPos < leavesLen ? leaves[leafPos++] : hashes[hashPos++];
            bytes32 b = flags[i]
                ? (leafPos < leavesLen ? leaves[leafPos++] : hashes[hashPos++])
                : proof[proofPos++];
            hashes[i] = _hashPair(a, b);
        }
        if (totalHashes > 0) return hashes[totalHashes - 1] == root;
        if (leavesLen > 0) return leaves[0] == root;
        return proof[0] == root;
    }
    function _hashPair(bytes32 a, bytes32 b) internal pure returns (bytes32) {
        return a < b ? keccak256(abi.encodePacked(a, b)) : keccak256(abi.encodePacked(b, a));
    }

    // --- tiny byte utils --------------------------------------------------
    function _popcount(uint8 x) internal pure returns (uint256 c) { for (; x != 0; x &= uint8(x - 1)) c++; }
    function _slice(bytes memory b, uint256 off, uint256 len) internal pure returns (bytes memory out) {
        out = new bytes(len);
        for (uint256 i = 0; i < len; i++) out[i] = b[off + i];
    }
    function _pad64(bytes memory x48) internal pure returns (bytes memory out) {
        out = new bytes(64);
        for (uint256 i = 0; i < 48; i++) out[16 + i] = x48[i];
    }
    function _gt48(bytes memory a, bytes memory b) internal pure returns (bool) {
        for (uint256 i = 0; i < 48; i++) {
            if (uint8(a[i]) > uint8(b[i])) return true;
            if (uint8(a[i]) < uint8(b[i])) return false;
        }
        return false;
    }
    function _copy32(bytes memory dst, uint256 off, bytes32 v) internal pure {
        for (uint256 i = 0; i < 32; i++) dst[off + i] = v[i];
    }
    function _copy32Bounded(bytes memory dst, uint256 off, bytes32 v, uint256 max) internal pure {
        uint256 m = max < 32 ? max : 32;
        for (uint256 i = 0; i < m; i++) dst[off + i] = v[i];
    }
}
