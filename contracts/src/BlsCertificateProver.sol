// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {B3Types, IB3FinalityProver} from "./IB3FinalityProver.sol";

/// EIP-2537 verifier for B3's stake-weighted BLS finality certificates.
///
/// The proof carries the signer bitmap, EIP-2537 uncompressed signature and
/// aggregate public-key witnesses, plus one ordered depth-13 membership path
/// for every absent validator. Ordered paths are intentional: B3 commits
/// members with keccak(left || right), not OpenZeppelin's commutative pair
/// hashing. The public witness ABI is:
///
/// abi.encode(
///   bytes bitmap,
///   bytes signatureG2Uncompressed,       // 256 bytes
///   bytes aggregatePubkeyG1Uncompressed, // 128 bytes
///   Absent[] absent
/// )
///
/// No validator key, weight, or production snapshot is hard-coded. Those are
/// public B3 chain facts committed by the verifier's Set_0 bootstrap and each
/// quorum-authorized successor header.
contract BlsCertificateProver is IB3FinalityProver {
    uint256 public constant SET_TREE_DEPTH = 13;

    address private constant G1ADD = address(0x0b);
    address private constant G2ADD = address(0x0d);
    address private constant PAIRING = address(0x0f);
    address private constant MAP_FP2_TO_G2 = address(0x11);
    address private constant MODEXP = address(0x05);

    bytes private constant FIELD_MODULUS =
        hex"1a0111ea397fe69a4b1ba7b6434bacd764774b84f38512bf6730d2a0f6b0f6241eabfffeb153ffffb9feffffffffaaab";

    // EIP-2537 uncompressed -(G1 generator): 16-byte pad || X ||
    // 16-byte pad || (p-Y). This replaces the empty placeholder that made
    // every pairing fail in the discarded draft.
    bytes private constant NEG_G1_GENERATOR =
        hex"0000000000000000000000000000000017f1d3a73197d7942695638c4fa9ac0fc3688c4f9774b905a14e3a3f171bac586c55e83ff97a1aeffb3af00adb22c6bb00000000000000000000000000000000114d1d6855d545a8aa7d76c8cf2e21f267816aef1db507c96655b9d5caac42364e6f38ba0ecb751bad54dcd6b939c2ca";

    bytes private constant SIGNATURE_DST =
        "BLS_SIG_BLS12381G2_XMD:SHA-256_SSWU_RO_POP_";

    struct Absent {
        uint32 index;
        bytes pubkey; // canonical 48-byte compressed G1
        uint64 weight;
        bytes uncompressedPubkey; // 128-byte EIP-2537 G1 witness
        bytes32[13] siblings; // ordered, leaf level first
    }

    function verify(
        bytes32 chainDomain,
        B3Types.FinalizedBlock calldata finalizedBlock,
        bytes32 signingSetHash,
        B3Types.SetHeader calldata signingSet,
        bytes calldata proof
    ) external view returns (bool) {
        if (
            chainDomain == bytes32(0) ||
            signingSetHash == bytes32(0) ||
            !B3Types.headerShapeValid(signingSet) ||
            B3Types.hashSetHeader(signingSet) != signingSetHash
        ) return false;

        (
            bytes memory bitmap,
            bytes memory signature,
            bytes memory aggregatePubkey,
            Absent[] memory absent
        ) = abi.decode(proof, (bytes, bytes, bytes, Absent[]));

        uint32 validatorCount = signingSet.validatorCount;
        if (bitmap.length != (uint256(validatorCount) + 7) / 8) return false;
        if (
            validatorCount % 8 != 0 &&
            (uint8(bitmap[validatorCount / 8]) >> (validatorCount % 8)) != 0
        ) return false;
        if (signature.length != 256 || aggregatePubkey.length != 128) {
            return false;
        }
        if (_isAllZero(signature)) return false;

        uint256 signerCount;
        for (uint256 i = 0; i < bitmap.length; ++i) {
            signerCount += _popcount(uint8(bitmap[i]));
        }
        if (signerCount == 0 || signerCount + absent.length != validatorCount) {
            return false;
        }
        // Ethereum deliberately applies a stricter bridge rule than B3's
        // stake-only finality: a certificate needs a >2/3 supermajority by
        // validator headcount as well as by stake weight. This makes an
        // initial two-member set 2-of-2 and prevents one very large staker
        // from authorizing bridge releases alone after the set grows.
        if (signerCount < headcountQuorum(validatorCount)) return false;

        uint256 absentWeight;
        uint256 previousIndex;
        for (uint256 i = 0; i < absent.length; ++i) {
            Absent memory member = absent[i];
            if (
                member.index >= validatorCount ||
                (i != 0 && member.index <= previousIndex) ||
                ((uint8(bitmap[member.index >> 3]) >> (member.index & 7)) & 1) != 0 ||
                member.pubkey.length != 48 ||
                member.uncompressedPubkey.length != 128 ||
                !_compressedG1Matches(member.uncompressedPubkey, member.pubkey) ||
                _memberRoot(
                    member.index,
                    member.pubkey,
                    member.weight,
                    member.siblings
                ) != signingSet.membersRoot
            ) return false;

            previousIndex = member.index;
            absentWeight += member.weight;
            if (absentWeight > signingSet.totalWeight) return false;
        }

        if (
            uint256(signingSet.totalWeight) - absentWeight <
            signingSet.quorumWeight
        ) return false;

        if (!_compressedG1Matches(aggregatePubkey, signingSet.aggregatePubkey)) {
            return false;
        }
        bytes memory signedAggregate = aggregatePubkey;
        for (uint256 i = 0; i < absent.length; ++i) {
            signedAggregate = _g1Add(
                signedAggregate,
                _g1Neg(absent[i].uncompressedPubkey)
            );
        }
        if (_isAllZero(signedAggregate)) return false;

        bytes32 digest = _finalityDigest(chainDomain, finalizedBlock);
        bytes memory messagePoint = _hashToG2(abi.encodePacked(digest));
        return _pairingTwo(
            signedAggregate,
            messagePoint,
            NEG_G1_GENERATOR,
            signature
        );
    }

    /// Public helper for relayers and cross-language vectors. Direction is
    /// index-sensitive at every level and therefore catches the old sorted-pair
    /// incompatibility.
    function validatorMemberRoot(
        uint32 index,
        bytes calldata pubkey,
        uint64 weight,
        bytes32[13] calldata siblings
    ) external pure returns (bytes32) {
        if (index >= (uint32(1) << 13) || pubkey.length != 48) {
            return bytes32(0);
        }
        return _memberRoot(index, pubkey, weight, siblings);
    }

    function negativeGenerator() external pure returns (bytes memory) {
        return NEG_G1_GENERATOR;
    }

    function headcountQuorum(uint32 validatorCount)
        public
        pure
        returns (uint256)
    {
        return (uint256(validatorCount) * 2) / 3 + 1;
    }

    function _memberRoot(
        uint32 index,
        bytes memory pubkey,
        uint64 weight,
        bytes32[13] memory siblings
    ) private pure returns (bytes32 node) {
        node = keccak256(abi.encodePacked(index, pubkey, weight));
        for (uint256 level = 0; level < SET_TREE_DEPTH; ++level) {
            node = ((uint256(index) >> level) & 1) == 0
                ? keccak256(abi.encodePacked(node, siblings[level]))
                : keccak256(abi.encodePacked(siblings[level], node));
        }
    }

    function _finalityDigest(
        bytes32 chainDomain,
        B3Types.FinalizedBlock calldata finalizedBlock
    ) private pure returns (bytes32) {
        bytes32 tagHash = sha256("B3/FINALITY/V1");
        return sha256(
            abi.encodePacked(
                tagHash,
                tagHash,
                chainDomain,
                finalizedBlock.height,
                finalizedBlock.blockHash,
                finalizedBlock.withdrawalRoot,
                finalizedBlock.validatorSetHash,
                finalizedBlock.epoch
            )
        );
    }

    function _g1Add(bytes memory a, bytes memory b)
        private
        view
        returns (bytes memory)
    {
        (bool ok, bytes memory result) = G1ADD.staticcall(
            abi.encodePacked(a, b)
        );
        require(ok && result.length == 128, "BLS_G1_ADD");
        return result;
    }

    function _g1Neg(bytes memory point)
        private
        pure
        returns (bytes memory out)
    {
        if (point.length != 128) return "";
        bytes memory negativeY = _subFieldPadded(_slice(point, 64, 64));
        out = new bytes(128);
        for (uint256 i = 0; i < 64; ++i) out[i] = point[i];
        for (uint256 i = 0; i < 64; ++i) out[64 + i] = negativeY[i];
    }

    function _pairingTwo(
        bytes memory p1,
        bytes memory q1,
        bytes memory p2,
        bytes memory q2
    ) private view returns (bool) {
        if (
            p1.length != 128 ||
            q1.length != 256 ||
            p2.length != 128 ||
            q2.length != 256
        ) return false;
        (bool ok, bytes memory result) = PAIRING.staticcall(
            abi.encodePacked(p1, q1, p2, q2)
        );
        return ok && result.length == 32 && uint256(bytes32(result)) == 1;
    }

    function _hashToG2(bytes memory message)
        private
        view
        returns (bytes memory)
    {
        bytes memory uniform = _expandMessageXmd(message, 256);
        bytes memory q0 = _mapFp2(_fieldElement(uniform, 0), _fieldElement(uniform, 1));
        bytes memory q1 = _mapFp2(_fieldElement(uniform, 2), _fieldElement(uniform, 3));
        (bool ok, bytes memory result) = G2ADD.staticcall(
            abi.encodePacked(q0, q1)
        );
        require(ok && result.length == 256, "BLS_G2_ADD");
        return result;
    }

    function _mapFp2(bytes memory c0, bytes memory c1)
        private
        view
        returns (bytes memory)
    {
        (bool ok, bytes memory result) = MAP_FP2_TO_G2.staticcall(
            abi.encodePacked(c0, c1)
        );
        require(ok && result.length == 256, "BLS_MAP_G2");
        return result;
    }

    function _fieldElement(bytes memory uniform, uint256 element)
        private
        view
        returns (bytes memory padded)
    {
        bytes memory chunk = _slice(uniform, element * 64, 64);
        bytes memory input = abi.encodePacked(
            uint256(64),
            uint256(1),
            uint256(48),
            chunk,
            bytes1(0x01),
            FIELD_MODULUS
        );
        (bool ok, bytes memory reduced) = MODEXP.staticcall(input);
        require(ok && reduced.length == 48, "BLS_MODEXP");
        padded = new bytes(64);
        for (uint256 i = 0; i < 48; ++i) padded[16 + i] = reduced[i];
    }

    function _expandMessageXmd(bytes memory message, uint256 length)
        private
        pure
        returns (bytes memory out)
    {
        uint256 blocks = (length + 31) / 32;
        require(blocks != 0 && blocks <= 255, "BLS_XMD_LENGTH");
        bytes memory dst = SIGNATURE_DST;
        bytes1 dstLength = bytes1(uint8(dst.length));
        bytes memory zeroPad = new bytes(64);
        bytes32 b0 = sha256(
            abi.encodePacked(
                zeroPad,
                message,
                bytes2(uint16(length)),
                bytes1(0),
                dst,
                dstLength
            )
        );
        bytes32 previous = sha256(
            abi.encodePacked(b0, bytes1(0x01), dst, dstLength)
        );
        out = new bytes(length);
        _copy32Bounded(out, 0, previous, length);
        for (uint256 i = 2; i <= blocks; ++i) {
            previous = sha256(
                abi.encodePacked(
                    b0 ^ previous,
                    bytes1(uint8(i)),
                    dst,
                    dstLength
                )
            );
            uint256 offset = (i - 1) * 32;
            _copy32Bounded(out, offset, previous, length - offset);
        }
    }

    function _compressedG1Matches(bytes memory uncompressed, bytes memory compressed)
        private
        pure
        returns (bool)
    {
        if (uncompressed.length != 128 || compressed.length != 48) return false;
        for (uint256 i = 0; i < 16; ++i) {
            if (uncompressed[i] != 0 || uncompressed[64 + i] != 0) return false;
        }

        uint8 flags = uint8(compressed[0]) & 0xe0;
        if ((flags & 0x80) == 0 || (flags & 0x40) != 0) return false;
        if (
            (uint8(compressed[0]) & 0x1f) !=
            (uint8(uncompressed[16]) & 0x1f)
        ) return false;
        for (uint256 i = 1; i < 48; ++i) {
            if (compressed[i] != uncompressed[16 + i]) return false;
        }

        bytes memory y = _slice(uncompressed, 80, 48);
        bytes memory pMinusY = _slice(
            _subFieldPadded(_pad64(y)),
            16,
            48
        );
        bool yIsLarger = _greaterThan48(y, pMinusY);
        return ((flags & 0x20) != 0) == yIsLarger;
    }

    function _subFieldPadded(bytes memory paddedY)
        private
        pure
        returns (bytes memory out)
    {
        bytes memory y = _slice(paddedY, 16, 48);
        bytes memory modulus = FIELD_MODULUS;
        bytes memory difference = new bytes(48);
        uint256 borrow;
        for (uint256 i = 48; i > 0; --i) {
            int256 value =
                int256(uint256(uint8(modulus[i - 1]))) -
                int256(uint256(uint8(y[i - 1]))) -
                int256(borrow);
            if (value < 0) {
                value += 256;
                borrow = 1;
            } else {
                borrow = 0;
            }
            difference[i - 1] = bytes1(uint8(uint256(value)));
        }
        out = _pad64(difference);
    }

    function _popcount(uint8 value) private pure returns (uint256 count) {
        while (value != 0) {
            value &= value - 1;
            ++count;
        }
    }

    function _isAllZero(bytes memory value) private pure returns (bool) {
        for (uint256 i = 0; i < value.length; ++i) {
            if (value[i] != 0) return false;
        }
        return true;
    }

    function _slice(bytes memory value, uint256 offset, uint256 length)
        private
        pure
        returns (bytes memory out)
    {
        if (offset + length > value.length) return "";
        out = new bytes(length);
        for (uint256 i = 0; i < length; ++i) out[i] = value[offset + i];
    }

    function _pad64(bytes memory value) private pure returns (bytes memory out) {
        if (value.length != 48) return "";
        out = new bytes(64);
        for (uint256 i = 0; i < 48; ++i) out[16 + i] = value[i];
    }

    function _greaterThan48(bytes memory a, bytes memory b)
        private
        pure
        returns (bool)
    {
        for (uint256 i = 0; i < 48; ++i) {
            if (uint8(a[i]) > uint8(b[i])) return true;
            if (uint8(a[i]) < uint8(b[i])) return false;
        }
        return false;
    }

    function _copy32Bounded(
        bytes memory destination,
        uint256 offset,
        bytes32 value,
        uint256 available
    ) private pure {
        uint256 count = available < 32 ? available : 32;
        for (uint256 i = 0; i < count; ++i) destination[offset + i] = value[i];
    }
}
