// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {B3Types} from "../src/IB3FinalityProver.sol";
import {BlsCertificateProver} from "../src/BlsCertificateProver.sol";

/// Cross-language vector generated with B3's vendored blst v0.3.17.
///
/// The signer uses the same deterministic IKM shape as BlsK(1) in B3's C++
/// finality tests. Its digest and compressed wire values come from
/// modern::FinalityDigest, bls::SecretKey::Sign and B3's vendored blst. The
/// vector covers EIP-2537 witness encoding, hash-to-G2 and the pairing check.
contract BlsCertificateVectorTest {
    bytes32 private constant CHAIN_DOMAIN = hex"d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0";
    bytes32 private constant BLOCK_HASH = hex"0101010101010101010101010101010101010101010101010101010101010101";
    bytes32 private constant WITHDRAWAL_ROOT = hex"0202020202020202020202020202020202020202020202020202020202020202";
    bytes32 private constant SUCCESSOR_SET_HASH = hex"0303030303030303030303030303030303030303030303030303030303030303";
    bytes32 private constant B3_BLST_DIGEST = hex"c02114826d2f2199a6bd2e186e51e25becd786c9a1b7464e7230cd76df098a65";

    bytes private constant COMPRESSED_PUBLIC_KEY =
        hex"aa3b6ee8aadb63c8e064dafdc3d0ae794ded037af26e60829da1592db0a237dc42684307339f176c31d97714fd78aed2";
    bytes private constant B3_COMPRESSED_SIGNATURE =
        hex"8f9775cab5513e55c48bb1bf625ca72be82529a074b01e461a020b0fe84b5a53eec6875b317f56febab3f84db1cc624b18f90a30ac5823ea45032e3e173bcc0e7d76d8aa4d59aff19492f15bae98a0d7adb1cd895adc4b804718a4138cee4c15";
    bytes private constant EIP2537_PUBLIC_KEY =
        hex"000000000000000000000000000000000a3b6ee8aadb63c8e064dafdc3d0ae794ded037af26e60829da1592db0a237dc42684307339f176c31d97714fd78aed2000000000000000000000000000000000d4611741ecb196805e3694d993b438dc6375d97e16430a57bf908c38482d9370a598e77552632acda8dec2033a7e4fc";
    bytes private constant EIP2537_SIGNATURE =
        hex"0000000000000000000000000000000018f90a30ac5823ea45032e3e173bcc0e7d76d8aa4d59aff19492f15bae98a0d7adb1cd895adc4b804718a4138cee4c15000000000000000000000000000000000f9775cab5513e55c48bb1bf625ca72be82529a074b01e461a020b0fe84b5a53eec6875b317f56febab3f84db1cc624b0000000000000000000000000000000004bca93e2a4de8c020d0a072e5ac55ede7e96da6c323d00b818a58196d9e33961a6e9a4508d5030c6b74f0fa02c69fcc0000000000000000000000000000000002758dc377d43dfe212ef15f1c18df6a8b3e87d34de3cbcd3375c8e52ee86bafe46757bf51018d349a44ce7eac8d528f";

    function test_B3BlstCertificateVerifiesViaEip2537() public {
        BlsCertificateProver prover = new BlsCertificateProver();
        B3Types.FinalizedBlock memory finalizedBlock = _finalizedBlock();
        B3Types.SetHeader memory signingSet = _signingSet();
        bytes32 signingSetHash = keccak256(B3Types.encodeSetHeader(signingSet));

        bytes32 tagHash = sha256("B3/FINALITY/V1");
        bytes32 digest = sha256(
            abi.encodePacked(
                tagHash,
                tagHash,
                CHAIN_DOMAIN,
                finalizedBlock.height,
                finalizedBlock.blockHash,
                finalizedBlock.withdrawalRoot,
                finalizedBlock.validatorSetHash,
                finalizedBlock.epoch
            )
        );
        require(digest == B3_BLST_DIGEST, "B3 digest vector");
        require(COMPRESSED_PUBLIC_KEY.length == 48, "B3 public-key wire size");
        require(B3_COMPRESSED_SIGNATURE.length == 96, "B3 signature wire size");

        BlsCertificateProver.Absent[] memory absent = new BlsCertificateProver.Absent[](0);
        bytes memory proof = abi.encode(hex"01", EIP2537_SIGNATURE, EIP2537_PUBLIC_KEY, absent);
        require(prover.verify(CHAIN_DOMAIN, finalizedBlock, signingSetHash, signingSet, proof), "B3 blst certificate");

        // The vector must prove a pairing, not merely exercise a live
        // precompile: the same signature cannot authenticate another block.
        finalizedBlock.blockHash = bytes32(uint256(0xdead));
        require(
            !prover.verify(CHAIN_DOMAIN, finalizedBlock, signingSetHash, signingSet, proof), "wrong finality digest"
        );
    }

    function _finalizedBlock() private pure returns (B3Types.FinalizedBlock memory finalizedBlock) {
        finalizedBlock.height = 811_001;
        finalizedBlock.blockHash = BLOCK_HASH;
        finalizedBlock.withdrawalRoot = WITHDRAWAL_ROOT;
        finalizedBlock.validatorSetHash = SUCCESSOR_SET_HASH;
        finalizedBlock.epoch = 7;
    }

    function _signingSet() private pure returns (B3Types.SetHeader memory signingSet) {
        signingSet.epoch = 7;
        signingSet.rulesetVersion = 1;
        signingSet.validatorCount = 1;
        signingSet.totalWeight = 333;
        signingSet.quorumWeight = 223;
        signingSet.aggregatePubkey = COMPRESSED_PUBLIC_KEY;

        bytes32 node = keccak256(abi.encodePacked(uint32(0), COMPRESSED_PUBLIC_KEY, uint64(333)));
        bytes32 zero;
        for (uint256 level = 0; level < 13; ++level) {
            node = keccak256(abi.encodePacked(node, zero));
            zero = keccak256(abi.encodePacked(zero, zero));
        }
        signingSet.membersRoot = node;
    }
}
