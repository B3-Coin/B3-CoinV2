// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {V2TestBase} from "./V2TestBase.sol";
import {MerkleTreeV2} from "./utils/MerkleTreeV2.sol";
import {B3TypesV2} from "../../src/v2/B3TypesV2.sol";
import {B3Types} from "../../src/IB3FinalityProver.sol";
import {B3BridgeAssetId} from "../../src/B3BridgeAssetId.sol";

/// External entry so a library revert can be expected from a sub-call.
contract B3TypesV2VectorHarness {
    function rootFromPath(bytes32 leaf, uint256 index, bytes32[32] memory path) external pure returns (bytes32) {
        return B3TypesV2.rootFromPath(leaf, index, path);
    }
}

/// Checks every entry of test/vectors/bridge_v2_vectors.json against
/// B3TypesV2 and MerkleTreeV2. The file comes from gen_v2_vectors.py (python
/// stdlib plus its own Keccak), so a pass means two independent
/// implementations agree; B3's C++ tests read the same file (RT4).
contract VectorsV2Test is V2TestBase {
    // Spec-documented values, pinned here too so a regenerated file cannot move them.
    bytes32 private constant SPEC_LEAF_V2 = 0x1cb905899ca0c488e158585c0208497d5415d1645035fbca3ca98ee2efb5ad64;
    bytes32 private constant SPEC_ASSET_ID_V2 = 0xdb9e32397f3656fb2317c7829429f430cfefcf665c08a26774cb7b2fb3c76fcd;
    bytes32 private constant STAND_IN_DOMAIN = 0x000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f;
    bytes32 private constant MAINNET_DOMAIN = 0xa651262ae9031646047f290513d77ccae0fa9bd4e5afe7e07155a08d5dd1486a;
    /// Display order, as getblockhash prints it (test_framework/b3bridge.py).
    bytes32 private constant REGTEST_MODERN_GENESIS_HASH =
        0x10d0f5bb6fde880011fd56ea35dddbf9b32da2a7825f5579ac587a7904d6929a;
    address private constant USDT = 0xdAC17F958D2ee523a2206206994597C13D831ec7;

    uint256 private constant LINEAGE_COUNT = 3;
    uint256 private constant SET_HEADER_COUNT = 3;
    uint256 private constant FINALIZED_COUNT = 3;
    uint256 private constant HISTORY_LEAF_COUNT = 12;
    uint256 private constant HISTORY_PATH_COUNT = 7;
    uint256 private constant WITHDRAWAL_LEAF_COUNT = 8;
    uint256 private constant WITHDRAWAL_PATH_COUNT = 2;

    function test_SchemaAndListLengths() public view {
        string memory j = readVectorsV2();
        assertEq(vm.parseJsonString(j, ".schema"), "b3-bridge-v2-vectors/1");
        _assertLength(j, ".lineage", "", LINEAGE_COUNT);
        _assertLength(j, ".set_header_v2", "", SET_HEADER_COUNT);
        _assertLength(j, ".finalized_block_v2", "", FINALIZED_COUNT);
        _assertLength(j, ".history.leaves", "", HISTORY_LEAF_COUNT);
        _assertLength(j, ".history.paths", "", HISTORY_PATH_COUNT);
        _assertLength(j, ".leaf_v2", "", 1);
        _assertLength(j, ".withdrawal_tree_v2.paths", "", WITHDRAWAL_PATH_COUNT);
        _assertLength(j, ".asset_id_v2", "", 1);
    }

    function test_TagConstantsMatchStrings() public view {
        string memory j = readVectorsV2();
        assertEq(B3TypesV2.FINALITY_TAG, sha256("B3/FINALITY/V2"), "finality tag");
        assertEq(B3TypesV2.LINEAGE_TAG, sha256("B3/FINALITY/LINEAGE/V2"), "lineage tag");
        assertEq(B3TypesV2.ASSET_TAG, sha256("B3/BRIDGE/ASSET/V2"), "asset tag");
        assertEq(B3TypesV2.LEAF_TAG_V2, keccak256("B3/BRIDGE/WITHDRAWAL/V2"), "leaf tag");

        assertEq(vm.parseJsonBytes32(j, ".tags.finality_v2"), B3TypesV2.FINALITY_TAG);
        assertEq(vm.parseJsonBytes32(j, ".tags.lineage_v2"), B3TypesV2.LINEAGE_TAG);
        assertEq(vm.parseJsonBytes32(j, ".tags.asset_v2"), B3TypesV2.ASSET_TAG);
        assertEq(vm.parseJsonBytes32(j, ".tags.leaf_v2"), B3TypesV2.LEAF_TAG_V2);
    }

    /// The generator's TaggedHash and LE64 machinery reproduces two committed
    /// V1 vectors that B3's C++ tests also assert.
    function test_V1Anchors() public view {
        string memory j = readVectorsV2();
        assertEq(
            B3BridgeAssetId.compute(
                vm.parseJsonBytes32(j, ".anchors.v1_asset_id.chain_domain"),
                uint64(vm.parseJsonUint(j, ".anchors.v1_asset_id.origin_chain_id")),
                vm.parseJsonAddress(j, ".anchors.v1_asset_id.vault"),
                vm.parseJsonAddress(j, ".anchors.v1_asset_id.token")
            ),
            vm.parseJsonBytes32(j, ".anchors.v1_asset_id.asset_id"),
            "V1 asset id anchor"
        );

        string memory p = ".anchors.v1_finality_digest";
        bytes32 tagHash = sha256("B3/FINALITY/V1");
        bytes32 digestV1 = sha256(
            abi.encodePacked(
                tagHash,
                tagHash,
                vm.parseJsonBytes32(j, string.concat(p, ".chain_domain")),
                uint64(vm.parseJsonUint(j, string.concat(p, ".height"))),
                vm.parseJsonBytes32(j, string.concat(p, ".block_hash")),
                vm.parseJsonBytes32(j, string.concat(p, ".withdrawal_root")),
                vm.parseJsonBytes32(j, string.concat(p, ".validator_set_hash")),
                uint64(vm.parseJsonUint(j, string.concat(p, ".epoch")))
            )
        );
        assertEq(digestV1, vm.parseJsonBytes32(j, string.concat(p, ".digest_v1")), "V1 digest anchor");
        // Same value as contracts/test/BlsCertificateVector.t.sol B3_BLST_DIGEST.
        assertEq(digestV1, 0xc02114826d2f2199a6bd2e186e51e25becd786c9a1b7464e7230cd76df098a65);
    }

    function test_LineageIds() public view {
        string memory j = readVectorsV2();
        bytes32[3] memory expectedDomains = [STAND_IN_DOMAIN, MAINNET_DOMAIN, _regtestChainDomain()];

        for (uint256 i = 0; i < LINEAGE_COUNT; ++i) {
            bytes32 domain = vm.parseJsonBytes32(j, jsonKey(".lineage", i, ".chain_domain"));
            uint64 genesisHeight = uint64(vm.parseJsonUint(j, jsonKey(".lineage", i, ".genesis_height")));
            bytes32 genesisBlockHash = vm.parseJsonBytes32(j, jsonKey(".lineage", i, ".genesis_block_hash"));
            bytes32 genesisSetHash = vm.parseJsonBytes32(j, jsonKey(".lineage", i, ".genesis_set_hash"));

            assertEq(domain, expectedDomains[i], "chain domain");
            assertEq(
                B3TypesV2.lineageId(domain, genesisHeight, genesisBlockHash, genesisSetHash),
                vm.parseJsonBytes32(j, jsonKey(".lineage", i, ".lineage_id")),
                "lineage id"
            );

            // Genesis is history leaf 0 and Set'_0 (finality dependency §6.2).
            assertEq(genesisHeight, vm.parseJsonUint(j, ".history.genesis_height"));
            assertEq(genesisBlockHash, vm.parseJsonBytes32(j, ".history.leaves[0].block_hash"));
            assertEq(genesisSetHash, vm.parseJsonBytes32(j, ".set_header_v2[0].set_hash"));
        }

        // The three domains give three lineages.
        bytes32 id0 = vm.parseJsonBytes32(j, ".lineage[0].lineage_id");
        bytes32 id1 = vm.parseJsonBytes32(j, ".lineage[1].lineage_id");
        bytes32 id2 = vm.parseJsonBytes32(j, ".lineage[2].lineage_id");
        assertTrue(id0 != id1 && id1 != id2 && id0 != id2, "distinct lineages");
    }

    function test_SetHeadersV2() public view {
        string memory j = readVectorsV2();
        for (uint256 i = 0; i < SET_HEADER_COUNT; ++i) {
            B3Types.SetHeader memory header = _setHeader(j, i);
            bytes memory encoded = vm.parseJsonBytes(j, jsonKey(".set_header_v2", i, ".encoded"));

            assertEq(header.epoch, i, "set epoch");
            assertEq(encoded.length, 110, "set header length");
            assertEq(B3Types.encodeSetHeader(header), encoded, "set header encoding");
            assertEq(
                B3Types.hashSetHeader(header),
                vm.parseJsonBytes32(j, jsonKey(".set_header_v2", i, ".set_hash")),
                "setHash"
            );
            assertEq(keccak256(encoded), B3Types.hashSetHeader(header));
            assertTrue(B3TypesV2.headerShapeValid(header), "V2 shape");
            assertFalse(B3Types.headerShapeValid(header), "V1 refuses ruleset 2");

            // Member tree (depth 13, ordered, zero padded).
            string memory members = jsonKey(".set_header_v2", i, ".members");
            uint256 count = header.validatorCount;
            _assertLength(j, members, "", count);
            bytes32[] memory leaves = new bytes32[](count);
            for (uint256 m = 0; m < count; ++m) {
                uint32 index = uint32(vm.parseJsonUint(j, jsonKey(members, m, ".index")));
                assertEq(index, m, "member order");
                leaves[m] = MerkleTreeV2.memberLeaf(
                    index,
                    vm.parseJsonBytes(j, jsonKey(members, m, ".pubkey")),
                    uint64(vm.parseJsonUint(j, jsonKey(members, m, ".weight")))
                );
                assertEq(leaves[m], vm.parseJsonBytes32(j, jsonKey(members, m, ".leaf")), "member leaf");
            }
            assertEq(MerkleTreeV2.root(leaves, count, MerkleTreeV2.MEMBER_DEPTH), header.membersRoot, "members root");
            for (uint256 m = 0; m < count; ++m) {
                bytes32[] memory siblings = vm.parseJsonBytes32Array(j, jsonKey(members, m, ".siblings"));
                bytes32[13] memory expected = MerkleTreeV2.path13(leaves, count, m);
                bytes32[13] memory got = MerkleTreeV2.toFixed13(siblings);
                for (uint256 l = 0; l < 13; ++l) {
                    assertEq(got[l], expected[l], "member sibling");
                }
                assertEq(MerkleTreeV2.rootFromPath(leaves[m], m, siblings), header.membersRoot, "member path");
            }
        }
    }

    function test_FinalizedBlocksV2() public view {
        string memory j = readVectorsV2();
        uint256 activation = vm.parseJsonUint(j, ".history.bridge_activation_height");
        bool sawBelow;
        bool sawAtActivation;
        bool sawFlagged;

        for (uint256 i = 0; i < FINALIZED_COUNT; ++i) {
            string memory p = jsonKey(".finalized_block_v2", i, "");
            B3TypesV2.FinalizedBlockV2 memory fb = _finalizedBlock(j, p);
            bytes32 domain = vm.parseJsonBytes32(j, string.concat(p, ".chain_domain"));
            bytes memory encoded = vm.parseJsonBytes(j, string.concat(p, ".encoded"));

            assertEq(encoded.length, B3TypesV2.FINALIZED_BLOCK_V2_LENGTH, "encoded length");
            assertEq(B3TypesV2.encode(fb), encoded, "encoding");
            assertEq(uint8(encoded[184]), fb.bridgeFlags, "flags byte");
            assertEq(B3TypesV2.digest(domain, fb), vm.parseJsonBytes32(j, string.concat(p, ".digest_v2")), "digest V2");
            assertEq(B3TypesV2.certId(fb), vm.parseJsonBytes32(j, string.concat(p, ".cert_id")), "certId");
            assertEq(B3TypesV2.certId(fb), keccak256(encoded));
            // The domain is bound by the digest, not by the certId.
            assertNotEq(B3TypesV2.digest(bytes32(uint256(domain) ^ 1), fb), B3TypesV2.digest(domain, fb));

            // Consistency with the other sections.
            uint256 lineage = vm.parseJsonUint(j, string.concat(p, ".lineage"));
            assertEq(domain, vm.parseJsonBytes32(j, jsonKey(".lineage", lineage, ".chain_domain")));
            assertEq(fb.lineageId, vm.parseJsonBytes32(j, jsonKey(".lineage", lineage, ".lineage_id")));
            assertEq(fb.height, vm.parseJsonUint(j, jsonKey(".history.leaves", fb.checkpointIndex, ".height")));
            assertEq(
                fb.blockHash, vm.parseJsonBytes32(j, jsonKey(".history.leaves", fb.checkpointIndex, ".block_hash"))
            );
            assertEq(
                fb.checkpointHistoryRoot, vm.parseJsonBytes32(j, jsonKey(".history.roots", fb.checkpointIndex, ""))
            );
            assertEq(fb.validatorSetHash, vm.parseJsonBytes32(j, jsonKey(".set_header_v2", fb.epoch + 1, ".set_hash")));
            assertGe(fb.checkpointIndex, 1);

            // Spec §2.4: zero root below V, the tree root from V.
            if (fb.height < activation) {
                assertEq(fb.withdrawalRoot, bytes32(0), "root below V");
                sawBelow = true;
            } else {
                assertEq(fb.withdrawalRoot, vm.parseJsonBytes32(j, ".withdrawal_tree_v2.root"), "root from V");
                if (fb.height == activation) sawAtActivation = true;
                if (fb.bridgeFlags & B3TypesV2.FLAG_DEPOSITS_CLOSED != 0) sawFlagged = true;
            }
        }
        assertTrue(sawBelow && sawAtActivation && sawFlagged, "case coverage");

        // The first case is the bootstrap shape: index 1, root 0, flags 0, epoch 0.
        B3TypesV2.FinalizedBlockV2 memory first = _finalizedBlock(j, ".finalized_block_v2[0]");
        assertEq(first.checkpointIndex, 1);
        assertEq(first.epoch, 0);
        assertEq(first.bridgeFlags, 0);
        assertEq(first.withdrawalRoot, bytes32(0));
    }

    function test_HistoryLeaves() public view {
        string memory j = readVectorsV2();
        uint256 genesisHeight = vm.parseJsonUint(j, ".history.genesis_height");
        uint256 interval = vm.parseJsonUint(j, ".history.interval");
        assertEq(genesisHeight, 261);
        assertEq(interval, 5);
        assertEq(vm.parseJsonUint(j, ".history.leaf_count"), HISTORY_LEAF_COUNT);

        for (uint256 i = 0; i < HISTORY_LEAF_COUNT; ++i) {
            uint64 height = uint64(vm.parseJsonUint(j, jsonKey(".history.leaves", i, ".height")));
            assertEq(vm.parseJsonUint(j, jsonKey(".history.leaves", i, ".index")), i);
            assertEq(height, genesisHeight + i * interval, "grid height");
            bytes32 leaf =
                B3TypesV2.historyLeaf(height, vm.parseJsonBytes32(j, jsonKey(".history.leaves", i, ".block_hash")));
            assertEq(leaf, vm.parseJsonBytes32(j, jsonKey(".history.leaves", i, ".leaf")), "history leaf");
        }
    }

    function test_HistoryRoots() public view {
        string memory j = readVectorsV2();
        bytes32[] memory leaves = _historyLeaves(j);
        bytes32[] memory roots = vm.parseJsonBytes32Array(j, ".history.roots");
        assertEq(roots.length, HISTORY_LEAF_COUNT + 1, "roots k = 0..N");
        assertEq(roots[0], MerkleTreeV2.zeros(32)[32], "empty root is zero_32");
        for (uint256 k = 0; k <= HISTORY_LEAF_COUNT; ++k) {
            assertEq(roots[k], MerkleTreeV2.root(leaves, k, 32), "history root");
        }
    }

    function test_HistoryPaths() public {
        string memory j = readVectorsV2();
        bytes32[] memory leaves = _historyLeaves(j);
        bytes32[] memory roots = vm.parseJsonBytes32Array(j, ".history.roots");
        bool sawLeaf0Bootstrap;
        bool sawAncestry;

        for (uint256 i = 0; i < HISTORY_PATH_COUNT; ++i) {
            string memory p = jsonKey(".history.paths", i, "");
            uint256 count = vm.parseJsonUint(j, string.concat(p, ".count"));
            uint256 index = vm.parseJsonUint(j, string.concat(p, ".index"));
            bytes32 leaf = vm.parseJsonBytes32(j, string.concat(p, ".leaf"));
            bytes32 root = vm.parseJsonBytes32(j, string.concat(p, ".root"));
            bytes32[] memory siblings = vm.parseJsonBytes32Array(j, string.concat(p, ".siblings"));

            assertLt(index, count);
            assertEq(leaf, leaves[index], "path leaf");
            assertEq(root, roots[count], "path root");
            bytes32[32] memory expected = MerkleTreeV2.path32(leaves, count, index);
            bytes32[32] memory path = MerkleTreeV2.toFixed32(siblings);
            for (uint256 l = 0; l < 32; ++l) {
                assertEq(path[l], expected[l], "history sibling");
            }
            assertEq(B3TypesV2.rootFromPath(leaf, index, path), root, "rootFromPath");
            // The same siblings at another index, or for another leaf, miss the root.
            assertNotEq(B3TypesV2.rootFromPath(leaf, index ^ 1, path), root);
            assertNotEq(B3TypesV2.rootFromPath(leaf ^ bytes32(uint256(1)), index, path), root);

            if (count == 1 && index == 0) sawLeaf0Bootstrap = true;
            if (index == count - 1 && count > 1) sawAncestry = true;
        }
        assertTrue(sawLeaf0Bootstrap && sawAncestry, "path coverage");

        // Index 2^32 is outside the depth-32 tree.
        B3TypesV2VectorHarness harness = new B3TypesV2VectorHarness();
        bytes32[32] memory leaf0Path = MerkleTreeV2.path32(leaves, 1, 0);
        assertEq(harness.rootFromPath(leaves[0], 0, leaf0Path), roots[1]);
        vm.expectRevert(B3TypesV2.IndexOutOfRange.selector);
        harness.rootFromPath(leaves[0], uint256(1) << 32, leaf0Path);
    }

    function test_LeafV2() public view {
        string memory j = readVectorsV2();
        string memory p = ".leaf_v2[0]";
        bytes32 asset = vm.parseJsonBytes32(j, string.concat(p, ".asset_id"));
        uint64 id = uint64(vm.parseJsonUint(j, string.concat(p, ".withdrawal_id")));
        uint64 chainId = uint64(vm.parseJsonUint(j, string.concat(p, ".chain_id")));
        address vault = vm.parseJsonAddress(j, string.concat(p, ".vault"));
        address token = vm.parseJsonAddress(j, string.concat(p, ".token"));
        address recipient = vm.parseJsonAddress(j, string.concat(p, ".recipient"));
        uint256 amount = vm.parseJsonUint(j, string.concat(p, ".amount"));
        uint64 b3Height = uint64(vm.parseJsonUint(j, string.concat(p, ".b3_height")));
        bytes memory preimage = vm.parseJsonBytes(j, string.concat(p, ".preimage"));

        // Spec §6.4 vector inputs.
        assertEq(asset, bytes32(0x1111111111111111111111111111111111111111111111111111111111111111));
        assertEq(id, 7);
        assertEq(chainId, 1);
        assertEq(vault, 0x2222222222222222222222222222222222222222);
        assertEq(token, USDT);
        assertEq(recipient, 0x3333333333333333333333333333333333333333);
        assertEq(amount, 1_000_000);
        assertEq(b3Height, 900_000);

        assertEq(preimage.length, B3TypesV2.WITHDRAWAL_LEAF_PREIMAGE_LENGTH, "preimage length");
        assertEq(
            preimage,
            abi.encodePacked(B3TypesV2.LEAF_TAG_V2, asset, id, chainId, vault, token, recipient, amount, b3Height),
            "preimage layout"
        );
        bytes32 leaf = B3TypesV2.withdrawalLeaf(asset, id, chainId, vault, token, recipient, amount, b3Height);
        assertEq(leaf, keccak256(preimage));
        assertEq(leaf, vm.parseJsonBytes32(j, string.concat(p, ".leaf")), "leaf V2 (json)");
        assertEq(leaf, SPEC_LEAF_V2, "leaf V2 (spec)");
    }

    function test_WithdrawalTreeV2() public view {
        string memory j = readVectorsV2();
        string memory p = ".withdrawal_tree_v2";
        bytes32 asset = vm.parseJsonBytes32(j, string.concat(p, ".asset_id"));
        uint64 chainId = uint64(vm.parseJsonUint(j, string.concat(p, ".chain_id")));
        address vault = vm.parseJsonAddress(j, string.concat(p, ".vault"));
        address token = vm.parseJsonAddress(j, string.concat(p, ".token"));
        address recipient = vm.parseJsonAddress(j, string.concat(p, ".recipient"));
        uint256 amount = vm.parseJsonUint(j, string.concat(p, ".amount"));
        uint64 b3Height = uint64(vm.parseJsonUint(j, string.concat(p, ".b3_height")));
        uint256 count = vm.parseJsonUint(j, string.concat(p, ".count"));
        bytes32[] memory leaves = vm.parseJsonBytes32Array(j, string.concat(p, ".leaves"));
        bytes32 root = vm.parseJsonBytes32(j, string.concat(p, ".root"));

        assertEq(count, WITHDRAWAL_LEAF_COUNT);
        assertEq(leaves.length, count);
        for (uint64 i = 0; i < count; ++i) {
            assertEq(
                leaves[i],
                B3TypesV2.withdrawalLeaf(asset, i, chainId, vault, token, recipient, amount, b3Height),
                "withdrawal leaf"
            );
        }
        assertEq(leaves[7], SPEC_LEAF_V2, "id 7 is the spec vector");
        assertEq(root, MerkleTreeV2.root(leaves, count, 32), "withdrawal root");
        assertNotEq(root, MerkleTreeV2.zeros(32)[32]);

        for (uint256 i = 0; i < WITHDRAWAL_PATH_COUNT; ++i) {
            string memory q = jsonKey(string.concat(p, ".paths"), i, "");
            uint256 index = vm.parseJsonUint(j, string.concat(q, ".index"));
            bytes32 leaf = vm.parseJsonBytes32(j, string.concat(q, ".leaf"));
            bytes32[32] memory path = MerkleTreeV2.toFixed32(vm.parseJsonBytes32Array(j, string.concat(q, ".siblings")));
            bytes32[32] memory expected = MerkleTreeV2.path32(leaves, count, index);
            for (uint256 l = 0; l < 32; ++l) {
                assertEq(path[l], expected[l], "withdrawal sibling");
            }
            assertEq(leaf, leaves[index]);
            assertEq(B3TypesV2.rootFromPath(leaf, index, path), root, "withdrawal path");
        }
    }

    function test_AssetIdV2() public view {
        string memory j = readVectorsV2();
        string memory p = ".asset_id_v2[0]";
        bytes32 domain = vm.parseJsonBytes32(j, string.concat(p, ".chain_domain"));
        uint64 originChainId = uint64(vm.parseJsonUint(j, string.concat(p, ".origin_chain_id")));
        address vault = vm.parseJsonAddress(j, string.concat(p, ".vault"));
        address token = vm.parseJsonAddress(j, string.concat(p, ".token"));
        bytes memory preimage = vm.parseJsonBytes(j, string.concat(p, ".preimage"));
        bytes32 asset = vm.parseJsonBytes32(j, string.concat(p, ".asset_id"));

        assertEq(domain, MAINNET_DOMAIN);
        assertEq(originChainId, 1);
        assertEq(vault, 0x2222222222222222222222222222222222222222);
        assertEq(token, USDT);

        // 147 bytes: tag ‖ tag ‖ domain ‖ 0x02 ‖ LE64(chain) ‖ vault ‖ token ‖ 6 ‖ 6.
        assertEq(preimage.length, B3TypesV2.ASSET_ID_PREIMAGE_LENGTH, "preimage length");
        assertEq(
            preimage,
            abi.encodePacked(
                B3TypesV2.ASSET_TAG,
                B3TypesV2.ASSET_TAG,
                domain,
                uint8(2),
                bytes8(0x0100000000000000), // LE64(1)
                vault,
                token,
                uint8(6),
                uint8(6)
            ),
            "preimage layout"
        );
        assertEq(sha256(preimage), asset);
        assertEq(B3TypesV2.assetId(domain, originChainId, vault, token), asset, "AssetId V2 (json)");
        assertEq(asset, SPEC_ASSET_ID_V2, "AssetId V2 (spec)");
        // Version byte 2 separates it from V1's id for the same inputs.
        assertNotEq(asset, B3BridgeAssetId.compute(domain, originChainId, vault, token));
    }

    /// The reference tree itself: zero table, root/path agreement at every
    /// position for small trees (empty positions included), the depth-13 root
    /// of contracts/test/BlsCertificateVector.t.sol, and agreement with
    /// B3TypesV2.rootFromPath at depth 32.
    function test_MerkleTreeV2SelfChecks() public pure {
        bytes32[] memory z = MerkleTreeV2.zeros(32);
        assertEq(z[0], bytes32(0));
        assertEq(z[1], keccak256(abi.encodePacked(bytes32(0), bytes32(0))));
        assertEq(z[2], keccak256(abi.encodePacked(z[1], z[1])));

        bytes32[] memory leaves = new bytes32[](9);
        for (uint256 i = 0; i < leaves.length; ++i) {
            leaves[i] = keccak256(abi.encodePacked("leaf", i));
        }
        for (uint256 count = 1; count <= leaves.length; ++count) {
            bytes32 root32 = MerkleTreeV2.root(leaves, count, 32);
            bytes32 root13 = MerkleTreeV2.root(leaves, count, 13);
            for (uint256 index = 0; index < count + 2; ++index) {
                bytes32 leaf = index < count ? leaves[index] : bytes32(0);
                bytes32[32] memory path32 = MerkleTreeV2.path32(leaves, count, index);
                assertEq(B3TypesV2.rootFromPath(leaf, index, path32), root32, "depth-32 path");
                assertEq(
                    MerkleTreeV2.rootFromPath(leaf, index, MerkleTreeV2.path(leaves, count, index, 13)),
                    root13,
                    "depth-13 path"
                );
            }
        }
        assertEq(MerkleTreeV2.root(leaves, 0, 32), z[32]);

        // contracts/test/BlsCertificateVector.t.sol builds its 1-member root by hand.
        bytes memory pubkey =
            hex"aa3b6ee8aadb63c8e064dafdc3d0ae794ded037af26e60829da1592db0a237dc42684307339f176c31d97714fd78aed2";
        bytes32 node = keccak256(abi.encodePacked(uint32(0), pubkey, uint64(333)));
        bytes32 zero;
        for (uint256 level = 0; level < 13; ++level) {
            node = keccak256(abi.encodePacked(node, zero));
            zero = keccak256(abi.encodePacked(zero, zero));
        }
        bytes32[] memory one = new bytes32[](1);
        one[0] = MerkleTreeV2.memberLeaf(0, pubkey, 333);
        assertEq(MerkleTreeV2.root(one, 1, 13), node, "V1 member root");
    }

    // -- helpers ---------------------------------------------------------------

    /// Entries 0 .. expected-1 exist and entry `expected` does not.
    function _assertLength(string memory j, string memory prefix, string memory suffix, uint256 expected) private view {
        for (uint256 i = 0; i < expected; ++i) {
            assertTrue(vm.keyExistsJson(j, jsonKey(prefix, i, suffix)), "missing entry");
        }
        assertFalse(vm.keyExistsJson(j, jsonKey(prefix, expected, suffix)), "unexpected extra entry");
    }

    function _setHeader(string memory j, uint256 i) private pure returns (B3Types.SetHeader memory header) {
        string memory p = jsonKey(".set_header_v2", i, "");
        header.epoch = uint64(vm.parseJsonUint(j, string.concat(p, ".epoch")));
        header.rulesetVersion = uint16(vm.parseJsonUint(j, string.concat(p, ".ruleset_version")));
        header.validatorCount = uint32(vm.parseJsonUint(j, string.concat(p, ".validator_count")));
        header.totalWeight = uint64(vm.parseJsonUint(j, string.concat(p, ".total_weight")));
        header.quorumWeight = uint64(vm.parseJsonUint(j, string.concat(p, ".quorum_weight")));
        header.aggregatePubkey = vm.parseJsonBytes(j, string.concat(p, ".aggregate_pubkey"));
        header.membersRoot = vm.parseJsonBytes32(j, string.concat(p, ".members_root"));
    }

    function _finalizedBlock(string memory j, string memory p)
        private
        pure
        returns (B3TypesV2.FinalizedBlockV2 memory fb)
    {
        fb.lineageId = vm.parseJsonBytes32(j, string.concat(p, ".lineage_id"));
        fb.height = uint64(vm.parseJsonUint(j, string.concat(p, ".height")));
        fb.blockHash = vm.parseJsonBytes32(j, string.concat(p, ".block_hash"));
        fb.withdrawalRoot = vm.parseJsonBytes32(j, string.concat(p, ".withdrawal_root"));
        fb.validatorSetHash = vm.parseJsonBytes32(j, string.concat(p, ".validator_set_hash"));
        fb.epoch = uint64(vm.parseJsonUint(j, string.concat(p, ".epoch")));
        fb.checkpointIndex = uint64(vm.parseJsonUint(j, string.concat(p, ".checkpoint_index")));
        fb.checkpointHistoryRoot = vm.parseJsonBytes32(j, string.concat(p, ".checkpoint_history_root"));
        fb.bridgeFlags = uint8(vm.parseJsonUint(j, string.concat(p, ".bridge_flags")));
    }

    function _historyLeaves(string memory j) private pure returns (bytes32[] memory leaves) {
        leaves = new bytes32[](HISTORY_LEAF_COUNT);
        for (uint256 i = 0; i < HISTORY_LEAF_COUNT; ++i) {
            leaves[i] = vm.parseJsonBytes32(j, jsonKey(".history.leaves", i, ".leaf"));
        }
    }

    /// modern::ModernChainDomain(genesis, genesis) on regtest:
    /// TaggedHash("B3/MODERN/CHAIN", g ‖ g) over B3's internal (reversed) byte order.
    function _regtestChainDomain() private pure returns (bytes32) {
        bytes32 internalOrder;
        for (uint256 i = 0; i < 32; ++i) {
            internalOrder |= bytes32(uint256(uint8(REGTEST_MODERN_GENESIS_HASH[i])) << (8 * i));
        }
        bytes32 tagHash = sha256("B3/MODERN/CHAIN");
        return sha256(abi.encodePacked(tagHash, tagHash, internalOrder, internalOrder));
    }
}
