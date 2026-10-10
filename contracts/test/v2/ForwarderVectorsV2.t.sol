// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {V2TestBase} from "./V2TestBase.sol";
import {B3Forwarder} from "../../src/v2/B3Forwarder.sol";
import {B3ForwarderTypesV2} from "../../src/v2/B3ForwarderTypesV2.sol";

/// Checks the "forwarder_v2" section of test/vectors/bridge_v2_vectors.json
/// against B3ForwarderTypesV2 (contracts spec §7.2, §8): the tags, every F_d
/// and F_x salt preimage (abi.encode), salt and CREATE2 account, the EIP-191
/// refund-EOA message, digest and signature, the 67-byte kind 6 exit block of
/// the burn record (node changes §7.2) and the OI-2 deposit fee caps. The file
/// comes from gen_v2_vectors.py (python stdlib, its own Keccak and secp256k1
/// recovery), so a pass means two independent implementations agree; B3's
/// src/modern/bridge_forwarder.h and src/bridge/eip191.h tests copy the same
/// values.
contract ForwarderVectorsV2Test is V2TestBase {
    string private constant SECTION = ".forwarder_v2";
    uint256 private constant DEPOSIT_COUNT = 3;
    uint256 private constant EXIT_COUNT = 3;
    uint256 private constant REFUND_MESSAGE_COUNT = 2;
    uint256 private constant FEE_CAP_COUNT = 4;
    /// LayerZero endpoint id of Arbitrum, the launch F_x factory's DST_EID.
    uint32 private constant EID_ARBITRUM = 30110;
    /// u8 exit_version ‖ u32 dst_eid ‖ to ‖ u16 bps ‖ u64 fee_cap ‖ refund_eoa.
    uint256 private constant EXIT_BLOCK_LENGTH = 67;
    uint8 private constant EXIT_BLOCK_VERSION = 1;

    function test_ForwarderListLengths() public view {
        string memory j = readVectorsV2();
        _assertLength(j, ".deposit", DEPOSIT_COUNT);
        _assertLength(j, ".exit", EXIT_COUNT);
        _assertLength(j, ".refund_eoa_message", REFUND_MESSAGE_COUNT);
        _assertLength(j, ".burn_exit_block", EXIT_COUNT);
        _assertLength(j, ".deposit_fee_cap", FEE_CAP_COUNT);
    }

    function test_ForwarderTagsMatchStrings() public view {
        string memory j = readVectorsV2();
        assertEq(B3ForwarderTypesV2.DEPOSIT_TAG, keccak256("B3/FWD/DEPOSIT/V2"), "deposit tag");
        assertEq(B3ForwarderTypesV2.EXIT_TAG, keccak256("B3/FWD/EXIT/OFT/V2"), "exit tag");
        assertEq(vm.parseJsonBytes32(j, _key(".tags.deposit")), B3ForwarderTypesV2.DEPOSIT_TAG);
        assertEq(vm.parseJsonBytes32(j, _key(".tags.exit")), B3ForwarderTypesV2.EXIT_TAG);
    }

    /// The vectors use a stand-in init-code hash; the real one commits to
    /// B3Forwarder's metadata and comes from the build (DeployV2 manifest).
    function test_InitCodeHashIsAStandIn() public view {
        string memory j = readVectorsV2();
        bytes32 initCodeHash = vm.parseJsonBytes32(j, _key(".init_code_hash"));
        assertEq(initCodeHash, keccak256("B3-VECTORS/forwarder-init-code"));
        assertNotEq(initCodeHash, keccak256(type(B3Forwarder).creationCode));
        assertEq(vm.parseJsonUint(j, _key(".pinned_dst_eid")), EID_ARBITRUM);
    }

    function test_DepositForwarderSaltsAndAccounts() public view {
        string memory j = readVectorsV2();
        bytes32 initCodeHash = vm.parseJsonBytes32(j, _key(".init_code_hash"));
        address[DEPOSIT_COUNT] memory accounts;
        for (uint256 i = 0; i < DEPOSIT_COUNT; ++i) {
            string memory p = jsonKey(_key(".deposit"), i, "");
            B3ForwarderTypesV2.DepositParams memory d = _depositParams(j, p);
            address factory = vm.parseJsonAddress(j, string.concat(p, ".factory"));

            bytes memory preimage = vm.parseJsonBytes(j, string.concat(p, ".salt_preimage"));
            assertEq(preimage.length, B3ForwarderTypesV2.SALT_PREIMAGE_LENGTH, "192-byte preimage");
            assertEq(
                preimage,
                abi.encode(B3ForwarderTypesV2.DEPOSIT_TAG, d.vault, d.b3Recipient, d.refundEOA, d.feeCap, d.notAfter),
                "preimage is abi.encode"
            );
            assertEq(B3ForwarderTypesV2.depositSaltPreimage(d), preimage, "library preimage");

            bytes32 salt = vm.parseJsonBytes32(j, string.concat(p, ".salt"));
            assertEq(keccak256(preimage), salt, "salt");
            assertEq(B3ForwarderTypesV2.depositSalt(d), salt, "library salt");

            address account = vm.parseJsonAddress(j, string.concat(p, ".account"));
            assertEq(B3ForwarderTypesV2.create2Address(factory, salt, initCodeHash), account, "library account");
            assertEq(_create2(factory, salt, initCodeHash), account, "85-byte CREATE2 preimage");

            assertTrue(B3ForwarderTypesV2.recipientV1Valid(d.b3Recipient), "RECIPIENT_V1");
            // OI-6: no F_d derivation yields the vault (or its factory).
            assertTrue(account != d.vault && account != factory, "account is neither vault nor factory");
            accounts[i] = account;
        }
        assertTrue(accounts[0] != accounts[1] && accounts[1] != accounts[2] && accounts[0] != accounts[2]);
    }

    function test_ExitForwarderSaltsAndAccounts() public view {
        string memory j = readVectorsV2();
        bytes32 initCodeHash = vm.parseJsonBytes32(j, _key(".init_code_hash"));
        uint32 pinned = uint32(vm.parseJsonUint(j, _key(".pinned_dst_eid")));
        address[EXIT_COUNT] memory accounts;
        for (uint256 i = 0; i < EXIT_COUNT; ++i) {
            string memory p = jsonKey(_key(".exit"), i, "");
            B3ForwarderTypesV2.ExitParams memory x = _exitParams(j, p);
            address factory = vm.parseJsonAddress(j, string.concat(p, ".factory"));

            bytes memory preimage = vm.parseJsonBytes(j, string.concat(p, ".salt_preimage"));
            assertEq(preimage.length, B3ForwarderTypesV2.SALT_PREIMAGE_LENGTH, "192-byte preimage");
            assertEq(
                preimage,
                abi.encode(B3ForwarderTypesV2.EXIT_TAG, x.dstEid, x.to, x.maxBridgeFeeBps, x.feeCap, x.refundEOA),
                "preimage is abi.encode"
            );
            assertEq(B3ForwarderTypesV2.exitSaltPreimage(x), preimage, "library preimage");

            bytes32 salt = vm.parseJsonBytes32(j, string.concat(p, ".salt"));
            assertEq(keccak256(preimage), salt, "salt");
            assertEq(B3ForwarderTypesV2.exitSalt(x), salt, "library salt");

            address account = vm.parseJsonAddress(j, string.concat(p, ".account"));
            assertEq(B3ForwarderTypesV2.create2Address(factory, salt, initCodeHash), account, "library account");
            assertEq(_create2(factory, salt, initCodeHash), account, "85-byte CREATE2 preimage");

            assertTrue(
                B3ForwarderTypesV2.exitParamsValid(x, pinned) == vm.parseJsonBool(j, string.concat(p, ".is_valid")),
                "isValid for the pinned Arbitrum eid"
            );
            accounts[i] = account;
        }
        assertTrue(accounts[0] != accounts[1] && accounts[1] != accounts[2] && accounts[0] != accounts[2]);
    }

    /// Every field is committed: changing any one moves the account.
    function test_EveryFieldMovesTheAccount() public view {
        string memory j = readVectorsV2();
        bytes32 initCodeHash = vm.parseJsonBytes32(j, _key(".init_code_hash"));

        string memory dp = jsonKey(_key(".deposit"), 0, "");
        B3ForwarderTypesV2.DepositParams memory d = _depositParams(j, dp);
        address fd = vm.parseJsonAddress(j, string.concat(dp, ".factory"));
        address dAccount = vm.parseJsonAddress(j, string.concat(dp, ".account"));
        for (uint256 field = 0; field < 7; ++field) {
            B3ForwarderTypesV2.DepositParams memory m = _depositParams(j, dp);
            address factory = fd;
            bytes32 hash = initCodeHash;
            if (field == 0) m.vault = address(uint160(d.vault) ^ 1);
            else if (field == 1) m.b3Recipient = bytes32(uint256(d.b3Recipient) ^ 1);
            else if (field == 2) m.refundEOA = address(uint160(d.refundEOA) ^ 1);
            else if (field == 3) m.feeCap = d.feeCap ^ 1;
            else if (field == 4) m.notAfter = d.notAfter ^ 1;
            else if (field == 5) factory = address(uint160(fd) ^ 1);
            else hash = initCodeHash ^ bytes32(uint256(1));
            assertTrue(
                B3ForwarderTypesV2.create2Address(factory, B3ForwarderTypesV2.depositSalt(m), hash) != dAccount,
                "deposit field moves the account"
            );
        }

        string memory xp = jsonKey(_key(".exit"), 0, "");
        B3ForwarderTypesV2.ExitParams memory x = _exitParams(j, xp);
        address fx = vm.parseJsonAddress(j, string.concat(xp, ".factory"));
        address xAccount = vm.parseJsonAddress(j, string.concat(xp, ".account"));
        for (uint256 field = 0; field < 5; ++field) {
            B3ForwarderTypesV2.ExitParams memory m = _exitParams(j, xp);
            if (field == 0) m.dstEid = x.dstEid ^ 1;
            else if (field == 1) m.to = bytes32(uint256(x.to) ^ 1);
            else if (field == 2) m.maxBridgeFeeBps = x.maxBridgeFeeBps ^ 1;
            else if (field == 3) m.feeCap = x.feeCap ^ 1;
            else m.refundEOA = address(uint160(x.refundEOA) ^ 1);
            assertTrue(
                B3ForwarderTypesV2.create2Address(fx, B3ForwarderTypesV2.exitSalt(m), initCodeHash) != xAccount,
                "exit field moves the account"
            );
        }
    }

    /// The text a wallet asks the refund EOA to sign (personal_sign), its
    /// EIP-191 digest and a real signature by anvil dev account 0 or 1.
    function test_RefundEoaMessagesAndSignatures() public view {
        string memory j = readVectorsV2();
        // Case 0 is regtest (lineage[2]), case 1 mainnet (lineage[1]); VectorsV2 pins both domains.
        uint256[REFUND_MESSAGE_COUNT] memory lineageOf = [uint256(2), 1];
        string[REFUND_MESSAGE_COUNT] memory names = ["regtest", "mainnet"];
        for (uint256 i = 0; i < REFUND_MESSAGE_COUNT; ++i) {
            string memory p = jsonKey(_key(".refund_eoa_message"), i, "");
            assertEq(vm.parseJsonString(j, string.concat(p, ".name")), names[i]);
            bytes32 domain = vm.parseJsonBytes32(j, string.concat(p, ".chain_domain"));
            assertEq(domain, vm.parseJsonBytes32(j, jsonKey(".lineage", lineageOf[i], ".chain_domain")), "domain");
            address eoa = vm.parseJsonAddress(j, string.concat(p, ".refund_eoa"));

            string memory message = vm.parseJsonString(j, string.concat(p, ".message"));
            assertEq(
                message,
                string.concat(
                    "B3 bridge V2 refund EOA\nchain domain: ",
                    vm.toString(domain),
                    "\nrefund EOA: ",
                    vm.toString(abi.encodePacked(eoa))
                )
            );

            bytes32 digest = keccak256(
                abi.encodePacked("\x19Ethereum Signed Message:\n", vm.toString(bytes(message).length), message)
            );
            assertEq(digest, vm.parseJsonBytes32(j, string.concat(p, ".eip191_digest")), "EIP-191 digest");

            bytes memory signature = vm.parseJsonBytes(j, string.concat(p, ".signature"));
            assertEq(signature.length, 65, "r || s || v");
            (bytes32 r, bytes32 s, uint8 v) = _split(signature);
            assertTrue(v == 27 || v == 28, "v is 27 or 28");
            assertLe(uint256(s), SECP256K1_HALF_N, "low s");
            assertEq(ecrecover(digest, v, r, s), eoa, "the refund EOA signed");
            // The other parity, or another message, recovers someone else.
            assertTrue(ecrecover(digest, 55 - v, r, s) != eoa, "flipped v");
            assertTrue(ecrecover(keccak256(abi.encodePacked(digest)), v, r, s) != eoa, "other digest");
        }
    }

    /// The burn record's exit block (node changes §7.2 item 5) for each exit
    /// case, and its recipient is that case's F_x account.
    function test_BurnExitBlocks() public view {
        string memory j = readVectorsV2();
        for (uint256 i = 0; i < EXIT_COUNT; ++i) {
            string memory p = jsonKey(_key(".burn_exit_block"), i, "");
            uint256 exitCase = vm.parseJsonUint(j, string.concat(p, ".exit_case"));
            assertEq(exitCase, i);
            string memory e = jsonKey(_key(".exit"), exitCase, "");
            B3ForwarderTypesV2.ExitParams memory x = _exitParams(j, e);
            assertEq(
                vm.parseJsonAddress(j, string.concat(p, ".recipient")),
                vm.parseJsonAddress(j, string.concat(e, ".account")),
                "recipient is F_x"
            );

            string memory f = string.concat(p, ".fields");
            assertEq(vm.parseJsonUint(j, string.concat(f, ".exit_version")), EXIT_BLOCK_VERSION);
            assertEq(vm.parseJsonUint(j, string.concat(f, ".dst_eid")), x.dstEid);
            assertEq(vm.parseJsonBytes32(j, string.concat(f, ".to")), x.to);
            assertEq(vm.parseJsonUint(j, string.concat(f, ".max_bridge_fee_bps")), x.maxBridgeFeeBps);
            uint256 feeCap = vm.parseJsonUint(j, string.concat(f, ".fee_cap"));
            assertEq(feeCap, x.feeCap);
            assertLe(feeCap, type(uint64).max, "B3 carries fee_cap as u64");
            assertEq(vm.parseJsonAddress(j, string.concat(f, ".refund_eoa")), x.refundEOA);

            bytes memory encoded = vm.parseJsonBytes(j, string.concat(p, ".encoded"));
            assertEq(encoded.length, EXIT_BLOCK_LENGTH, "67 bytes");
            assertEq(
                encoded,
                abi.encodePacked(EXIT_BLOCK_VERSION, x.dstEid, x.to, x.maxBridgeFeeBps, uint64(feeCap), x.refundEOA),
                "packed big-endian exit block"
            );
        }
    }

    /// OI-2: one delivery of at most MAX_DEPOSIT_RAW per forwarder, so one
    /// flush moves it; feeCap = fee_per_tx × 4 pays deploy, two flushes and
    /// a returnToOwner.
    function test_DepositFeeCapsOI2() public view {
        string memory j = readVectorsV2();
        uint256 feePerTx;
        uint256 feeCap;
        for (uint256 i = 0; i < FEE_CAP_COUNT; ++i) {
            string memory p = jsonKey(_key(".deposit_fee_cap"), i, "");
            feePerTx = vm.parseJsonUint(j, string.concat(p, ".fee_per_tx"));
            feeCap = vm.parseJsonUint(j, string.concat(p, ".fee_cap"));
            assertGt(feePerTx, 0);
            assertEq(feeCap, feePerTx * 4, "fee cap");
            assertLe(feeCap, type(uint64).max, "fits u64 and the uint96 feeCap");
        }
        assertTrue((feePerTx + 1) * 4 > type(uint64).max, "the last case is the largest fee_per_tx that fits u64");
    }

    // ============================================================== helpers

    uint256 private constant SECP256K1_HALF_N = 0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF5D576E7357A4501DDFE92F46681B20A0;

    function _key(string memory suffix) private pure returns (string memory) {
        return string.concat(SECTION, suffix);
    }

    function _assertLength(string memory j, string memory list, uint256 expected) private view {
        string memory prefix = _key(list);
        for (uint256 i = 0; i < expected; ++i) {
            assertTrue(vm.keyExistsJson(j, jsonKey(prefix, i, "")), "missing entry");
        }
        assertFalse(vm.keyExistsJson(j, jsonKey(prefix, expected, "")), "unexpected extra entry");
    }

    function _depositParams(string memory j, string memory p)
        private
        pure
        returns (B3ForwarderTypesV2.DepositParams memory d)
    {
        d.vault = vm.parseJsonAddress(j, string.concat(p, ".vault"));
        d.b3Recipient = vm.parseJsonBytes32(j, string.concat(p, ".b3_recipient"));
        d.refundEOA = vm.parseJsonAddress(j, string.concat(p, ".refund_eoa"));
        uint256 feeCap = vm.parseJsonUint(j, string.concat(p, ".fee_cap"));
        uint256 notAfter = vm.parseJsonUint(j, string.concat(p, ".not_after"));
        assertLe(feeCap, type(uint64).max, "B3 carries fee_cap as u64");
        assertLe(notAfter, type(uint64).max, "notAfter is uint64");
        d.feeCap = uint96(feeCap);
        d.notAfter = uint64(notAfter);
    }

    function _exitParams(string memory j, string memory p)
        private
        pure
        returns (B3ForwarderTypesV2.ExitParams memory x)
    {
        uint256 dstEid = vm.parseJsonUint(j, string.concat(p, ".dst_eid"));
        uint256 bps = vm.parseJsonUint(j, string.concat(p, ".max_bridge_fee_bps"));
        uint256 feeCap = vm.parseJsonUint(j, string.concat(p, ".fee_cap"));
        assertLe(dstEid, type(uint32).max, "dstEid is uint32");
        assertLe(bps, type(uint16).max, "maxBridgeFeeBps is uint16");
        assertLe(feeCap, type(uint64).max, "B3 carries fee_cap as u64");
        x.dstEid = uint32(dstEid);
        x.to = vm.parseJsonBytes32(j, string.concat(p, ".to"));
        x.maxBridgeFeeBps = uint16(bps);
        x.feeCap = uint96(feeCap);
        x.refundEOA = vm.parseJsonAddress(j, string.concat(p, ".refund_eoa"));
    }

    /// EIP-1014 from its 85-byte preimage, independent of the library.
    function _create2(address factory, bytes32 salt, bytes32 initCodeHash) private pure returns (address) {
        bytes memory preimage = abi.encodePacked(bytes1(0xff), factory, salt, initCodeHash);
        assertEq(preimage.length, B3ForwarderTypesV2.CREATE2_PREIMAGE_LENGTH, "85-byte preimage");
        return address(uint160(uint256(keccak256(preimage))));
    }

    function _split(bytes memory signature) private pure returns (bytes32 r, bytes32 s, uint8 v) {
        assembly ("memory-safe") {
            r := mload(add(signature, 0x20))
            s := mload(add(signature, 0x40))
            v := byte(0, mload(add(signature, 0x60)))
        }
    }
}
