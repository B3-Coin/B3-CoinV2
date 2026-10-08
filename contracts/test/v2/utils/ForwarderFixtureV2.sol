// SPDX-License-Identifier: MIT
pragma solidity ^0.8.28;

import {V2TestBase, VmV2} from "../V2TestBase.sol";
import {RuntimeScanV2 as Scan} from "./RuntimeScanV2.sol";
import {MockVerifierV2} from "../mocks/MockVerifierV2.sol";
import {B3VaultV2} from "../../../src/v2/B3VaultV2.sol";
import {IB3FinalityVerifierV2} from "../../../src/v2/IB3FinalityVerifierV2.sol";
import {B3Forwarder} from "../../../src/v2/B3Forwarder.sol";
import {B3ForwarderTypesV2} from "../../../src/v2/B3ForwarderTypesV2.sol";
import {B3DepositForwarderFactory} from "../../../src/v2/B3DepositForwarderFactory.sol";
import {B3OftExitFactory} from "../../../src/v2/B3OftExitFactory.sol";
import {IOFTV2} from "../../../src/v2/IOFTV2.sol";
import {MockTetherToken} from "../../../regtest/MockTetherToken.sol";
import {MockLzEndpoint} from "../../../regtest/MockLzEndpoint.sol";
import {MockOFTAdapter} from "../../../regtest/MockOFTAdapter.sol";
import {MockUSDT0} from "../../../regtest/MockUSDT0.sol";

/// Cheat codes the forwarder suites need beyond VmV2.
interface ForwarderCheatsV2 {
    function getNonce(address account) external view returns (uint64 nonce);
    function computeCreateAddress(address deployer, uint256 nonce) external pure returns (address);
    function expectCall(address callee, uint256 msgValue, bytes calldata data) external;
}

/// LayerZero packets of the regtest mock endpoints (test only): what the
/// harness LayerZero relayer reads from PacketSent on the source endpoint and
/// hands to the destination endpoint's `deliver`.
library LzPacketsV2 {
    struct Packet {
        address endpoint; // source endpoint, the PacketSent emitter
        uint32 srcEid; // its endpoint id
        uint64 nonce;
        uint32 dstEid;
        address sender; // source OApp
        bytes32 receiver; // destination OApp word
        bytes message; // abi.encodePacked(bytes32 to, uint64 amount)
    }

    /// Every PacketSent `endpoint` emitted in `logs`, in order.
    function sent(VmV2.Log[] memory logs, address endpoint) internal view returns (Packet[] memory packets) {
        uint32 srcEid = MockLzEndpoint(endpoint).eid();
        bytes32 topic = MockLzEndpoint.PacketSent.selector;
        uint256 n = 0;
        for (uint256 i = 0; i < logs.length; ++i) {
            if (logs[i].emitter == endpoint && logs[i].topics.length == 4 && logs[i].topics[0] == topic) ++n;
        }
        packets = new Packet[](n);
        n = 0;
        for (uint256 i = 0; i < logs.length; ++i) {
            VmV2.Log memory log = logs[i];
            if (log.emitter != endpoint || log.topics.length != 4 || log.topics[0] != topic) continue;
            (bytes32 receiver, bytes memory message) = abi.decode(log.data, (bytes32, bytes));
            packets[n++] = Packet({
                endpoint: endpoint,
                srcEid: srcEid,
                // casting is safe: indexed uint64, uint32 and address topics are zero-extended
                // forge-lint: disable-next-line(unsafe-typecast)
                nonce: uint64(uint256(log.topics[1])),
                // forge-lint: disable-next-line(unsafe-typecast)
                dstEid: uint32(uint256(log.topics[2])),
                sender: address(uint160(uint256(log.topics[3]))),
                receiver: receiver,
                message: message
            });
        }
    }

    /// The single PacketSent `endpoint` emitted in `logs`.
    function only(VmV2.Log[] memory logs, address endpoint) internal view returns (Packet memory) {
        Packet[] memory packets = sent(logs, endpoint);
        require(packets.length == 1, "exactly one packet");
        return packets[0];
    }

    /// Delivers `pk` at `dst` (the harness relayer's call). It makes exactly
    /// one external call, so vm.prank and vm.expectRevert apply to it.
    function deliver(Packet memory pk, MockLzEndpoint dst) internal {
        dst.deliver(
            pk.srcEid,
            bytes32(uint256(uint160(pk.sender))),
            pk.nonce,
            address(uint160(uint256(pk.receiver))),
            pk.message
        );
    }

    /// The OFT message's recipient word.
    function to(Packet memory pk) internal pure returns (bytes32 word) {
        bytes memory m = pk.message;
        require(m.length == 40, "OFT message length");
        assembly ("memory-safe") {
            word := mload(add(m, 0x20))
        }
    }

    /// The OFT message's amount.
    function amount(Packet memory pk) internal pure returns (uint256) {
        bytes memory m = pk.message;
        require(m.length == 40, "OFT message length");
        bytes32 tail;
        assembly ("memory-safe") {
            tail := mload(add(m, 0x40))
        }
        return uint256(tail) >> 192;
    }
}

/// Shared fixture of the forwarder suites (test only; contracts spec §7-§8,
/// §12; regtest plan §4.1 and RT6), on one in-process EVM standing for both
/// anvils:
///
/// - "Ethereum" (31337): MockVerifierV2 (depositsOpen set by the test),
///   MockTetherToken, B3VaultV2 and B3DepositForwarderFactory, deployed as
///   DeployV2 does: the vault pins the factory's predicted address (the test
///   contract's next CREATE after the vault), then the factory is created
///   there and checks the vault. Also MockLzEndpoint(30101), the
///   MockOFTAdapter over the USDT, and B3OftExitFactory pinned to it with
///   DST_EID 30110.
/// - "Arbitrum" (31338): MockLzEndpoint(30110) and MockUSDT0, peered with
///   the adapter.
///
/// The test contract owns the token, the endpoints and the OFT mocks. Time
/// starts at NOW; the default notAfter is 30 days later (spec §7.4).
abstract contract ForwarderFixtureV2 is V2TestBase {
    using LzPacketsV2 for LzPacketsV2.Packet;

    ForwarderCheatsV2 internal constant CHEATS =
        ForwarderCheatsV2(address(uint160(uint256(keccak256("hevm cheat code")))));

    bytes32 internal constant DOMAIN = keccak256("B3/TEST/BRIDGE/V2/FORWARDER-DOMAIN");
    uint64 internal constant V = 311;
    uint256 internal constant UNIT = 1_000_000;
    /// The vault's MAX_DEPOSIT_RAW: 10,000 USDT (B-14).
    uint256 internal constant CAP = 10_000_000_000;
    uint256 internal constant NOW = 1_800_000_000;
    /// NOW + 30 days.
    uint64 internal constant NOT_AFTER = 1_802_592_000;
    /// A relayer's per-transaction fee in the suites (0.25 USDT).
    uint256 internal constant FEE = 250_000;
    /// Spec §7.4's default F_d cap: deploy, two flushes and a returnToOwner.
    uint96 internal constant DEFAULT_FEE_CAP = 1_000_000;
    /// exitParams()' per-forward fee cap (5 USDT).
    uint96 internal constant EXIT_FEE_CAP = 5_000_000;

    uint32 internal constant ETH_EID = 30101;
    uint32 internal constant ARB_EID = 30110;
    /// Opaque executor options F_x passes through (a type-3 lzReceive gas option).
    bytes internal constant EXTRA_OPTIONS = hex"00030100110100000000000000000000000000030d40";

    address internal constant ALICE = address(0xA11CE0); // user; default refundEOA
    address internal constant BOB = address(0xB0B0B0); // user on Arbitrum
    address internal constant CAROL = address(0xCA201C); // refund target
    address internal constant RELAYER = address(0x5E1A7E01);
    address internal constant RELAYER2 = address(0x5E1A7E02);
    /// RECIPIENT_V1: 11 zero bytes, version 1, a 20-byte B3 key hash.
    bytes32 internal constant B3_RECIPIENT =
        bytes32((uint256(1) << 160) | uint256(uint160(0x5151515151515151515151515151515151515151)));

    /// keccak256("Deposit(uint64,address,uint256,bytes32)"), V1's topic 0.
    bytes32 internal constant DEPOSIT_TOPIC = 0xdaf0af297d25c0e96a0b209d35692b4e07c503634eeca57fc5c35c006acf527f;
    /// keccak256("Transfer(address,address,uint256)").
    bytes32 internal constant TRANSFER_TOPIC = 0xddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef;

    uint256 internal constant EIP170_RUNTIME_LIMIT = 24_576;
    uint256 internal constant EIP3860_INITCODE_LIMIT = 49_152;

    MockVerifierV2 internal verifier;
    MockTetherToken internal usdt;
    B3VaultV2 internal vault;
    B3DepositForwarderFactory internal fd;
    MockLzEndpoint internal endpointEth;
    MockLzEndpoint internal endpointArb;
    MockOFTAdapter internal adapter;
    MockUSDT0 internal usdt0;
    B3OftExitFactory internal fx;

    function setUp() public virtual {
        vm.warp(NOW);
        verifier = new MockVerifierV2(DOMAIN, V);
        verifier.setDepositsOpen(true);
        usdt = new MockTetherToken();

        address predicted = CHEATS.computeCreateAddress(address(this), uint256(CHEATS.getNonce(address(this))) + 1);
        vault = new B3VaultV2(
            B3VaultV2.Config({
                verifier: IB3FinalityVerifierV2(address(verifier)),
                verifierCodeHash: address(verifier).codehash,
                b3ChainDomain: DOMAIN,
                originToken: address(usdt),
                tokenCodeHash: address(usdt).codehash,
                forwarderFactory: predicted,
                maxDepositRaw: CAP,
                predecessor: address(0),
                predecessorAssetId: bytes32(0)
            })
        );
        fd = new B3DepositForwarderFactory(address(vault), address(vault).codehash);
        require(address(fd) == predicted, "F_d factory at the predicted address");

        endpointEth = new MockLzEndpoint(ETH_EID);
        endpointArb = new MockLzEndpoint(ARB_EID);
        adapter = new MockOFTAdapter(address(usdt), address(endpointEth));
        usdt0 = new MockUSDT0(address(endpointArb));
        adapter.setPeer(ARB_EID, word(address(usdt0)));
        usdt0.setPeer(ETH_EID, word(address(adapter)));
        fx = new B3OftExitFactory(address(usdt), address(adapter), ARB_EID, EXTRA_OPTIONS);

        vm.label(address(vault), "vault");
        vm.label(address(fd), "F_d factory");
        vm.label(address(fx), "F_x factory");
        vm.label(address(adapter), "OFT adapter");
        vm.label(address(usdt0), "USDT0");
    }

    // ============================================================== params

    function depositParams() internal view returns (B3ForwarderTypesV2.DepositParams memory) {
        return depositParamsWith(DEFAULT_FEE_CAP, NOT_AFTER);
    }

    function depositParamsWith(uint96 feeCap, uint64 notAfter)
        internal
        view
        returns (B3ForwarderTypesV2.DepositParams memory)
    {
        return B3ForwarderTypesV2.DepositParams({
            vault: address(vault), b3Recipient: B3_RECIPIENT, refundEOA: ALICE, feeCap: feeCap, notAfter: notAfter
        });
    }

    /// Withdrawal to BOB on Arbitrum, 0.5% slippage bound, 5 USDT fee cap.
    function exitParams() internal pure returns (B3ForwarderTypesV2.ExitParams memory) {
        return B3ForwarderTypesV2.ExitParams({
            dstEid: ARB_EID, to: word(BOB), maxBridgeFeeBps: 50, feeCap: EXIT_FEE_CAP, refundEOA: ALICE
        });
    }

    // ================================================================ helpers

    function word(address a) internal pure returns (bytes32) {
        return bytes32(uint256(uint160(a)));
    }

    /// Issues `amount` USDT to `to` (the test contract owns the token).
    function fundUsdt(address to, uint256 amount) internal {
        usdt.issue(amount);
        usdt.transfer(to, amount);
    }

    /// A flush as the top-level call of the codeless EOA `relayer` (PR-2).
    function flushAs(address relayer, B3ForwarderTypesV2.DepositParams memory p, uint256 fee)
        internal
        returns (uint256)
    {
        vm.prank(relayer, relayer);
        return fd.flush(p, fee);
    }

    function expectFlushRevert(address relayer, B3ForwarderTypesV2.DepositParams memory p, uint256 fee, bytes4 selector)
        internal
    {
        vm.expectRevert(selector);
        vm.prank(relayer, relayer);
        fd.flush(p, fee);
    }

    /// Route (b) on the Arbitrum side: `user` holds `amount` USDT0 (minted)
    /// and sends it with the OFT to `to` on Ethereum (spec §7.4: minAmountLD
    /// = amountLD, no compose). Returns the packet the endpoint emitted.
    function sendFromArbitrum(address user, bytes32 to, uint256 amount)
        internal
        returns (LzPacketsV2.Packet memory pk)
    {
        usdt0.mint(user, amount);
        uint256 nativeFee = endpointArb.quote();
        vm.deal(user, nativeFee);
        IOFTV2.SendParam memory sp = IOFTV2.SendParam({
            dstEid: ETH_EID, to: to, amountLD: amount, minAmountLD: amount, extraOptions: "", composeMsg: "", oftCmd: ""
        });
        vm.recordLogs();
        vm.prank(user, user);
        usdt0.send{value: nativeFee}(sp, IOFTV2.MessagingFee(nativeFee, 0), user);
        pk = LzPacketsV2.only(vm.getRecordedLogs(), address(endpointArb));
    }

    /// Route (b) end to end: USDT0 from `user` on Arbitrum, delivered by the
    /// harness relayer, paid in USDT by the adapter to `to` on Ethereum. The
    /// adapter is seeded with the backing first.
    function bridgeFromArbitrum(address user, address to, uint256 amount) internal {
        fundUsdt(address(adapter), amount);
        LzPacketsV2.Packet memory pk = sendFromArbitrum(user, word(to), amount);
        pk.deliver(endpointEth);
    }

    // ======================================================= runtime checks

    /// EIP-170 / EIP-3860 bounds.
    function assertSizes(string memory name, uint256 runtime, uint256 initcode) internal pure {
        assertGt(runtime, 0);
        assertLe(runtime, EIP170_RUNTIME_LIMIT, string.concat(name, ": EIP-170 runtime limit"));
        assertLe(initcode, EIP3860_INITCODE_LIMIT, string.concat(name, ": EIP-3860 initcode limit"));
    }

    /// Opcode counts of a forwarder factory's own code. The runtime embeds
    /// B3Forwarder's creation code as data (for CREATE2), right before the
    /// factory's CBOR metadata, and that data is not code: a linear scan
    /// would read its metadata bytes as opcodes. The embedded copy must be
    /// byte-identical to type(B3Forwarder).creationCode and end exactly where
    /// the metadata starts, so the scan covers everything else.
    function factoryOpcodeCounts(bytes memory code) internal pure returns (uint256[256] memory counts, uint256 total) {
        bytes memory embedded = type(B3Forwarder).creationCode;
        uint256 end = Scan.executableEnd(code);
        require(end > embedded.length, "embedded init code");
        uint256 start = end - embedded.length;
        bytes32 window;
        assembly ("memory-safe") {
            window := keccak256(add(add(code, 0x20), start), mload(embedded))
        }
        assertEq(window, keccak256(embedded), "B3Forwarder init code sits before the metadata");
        for (uint256 pc = 0; pc < start; ++pc) {
            uint8 op = uint8(code[pc]);
            ++counts[op];
            ++total;
            if (op >= 0x60 && op <= 0x7f) pc += op - 0x5f;
        }
    }

    /// No NUMBER, call-code, delegate call, CREATE or SELFDESTRUCT in a
    /// factory; CREATE2 present (it deploys accounts).
    function assertFactoryRuntimeClean(string memory name, bytes memory code) internal pure {
        (uint256[256] memory counts, uint256 total) = factoryOpcodeCounts(code);
        assertGt(total, 100);
        assertEq(counts[Scan.NUMBER], 0, string.concat(name, ": no NUMBER"));
        assertEq(counts[Scan.CALLCODE] + counts[Scan.DELEGATECALL], 0, string.concat(name, ": no delegation"));
        assertEq(counts[Scan.CREATE] + counts[Scan.SELFDESTRUCT], 0, string.concat(name, ": no CREATE/SELFDESTRUCT"));
        assertGt(counts[Scan.CREATE2], 0);
    }

    /// B3Forwarder: the deployed runtime and the constructor part of its init
    /// code are free of NUMBER, storage, delegation and creation opcodes.
    function assertForwarderRuntimeClean(address account) internal view {
        bytes memory runtime = account.code;
        (uint256[256] memory counts, uint256 total) = Scan.opcodeCounts(runtime);
        assertGt(total, 10);
        assertEq(counts[Scan.NUMBER], 0, "B3Forwarder: no NUMBER");
        assertEq(counts[Scan.SLOAD] + counts[Scan.SSTORE], 0, "B3Forwarder: no storage");
        assertEq(counts[Scan.TLOAD] + counts[Scan.TSTORE], 0, "B3Forwarder: no transient storage");
        assertEq(counts[Scan.CALLCODE] + counts[Scan.DELEGATECALL], 0, "B3Forwarder: no delegation");
        assertEq(counts[Scan.CREATE] + counts[Scan.CREATE2] + counts[Scan.SELFDESTRUCT], 0, "B3Forwarder: no creation");

        // Init code = constructor ‖ runtime template (FACTORY immutable zero).
        bytes memory creation = type(B3Forwarder).creationCode;
        require(creation.length > runtime.length, "init code");
        uint256 constructorEnd = creation.length - runtime.length;
        uint256 initNumber = 0;
        for (uint256 pc = 0; pc < constructorEnd; ++pc) {
            uint8 op = uint8(creation[pc]);
            if (op == Scan.NUMBER) ++initNumber;
            if (op >= 0x60 && op <= 0x7f) pc += op - 0x5f;
        }
        assertEq(initNumber, 0, "B3Forwarder constructor: no NUMBER");
        // The template's trailing metadata equals the deployed runtime's.
        uint256 metaLength = runtime.length - Scan.executableEnd(runtime);
        for (uint256 i = 1; i <= metaLength; ++i) {
            assertEq(uint256(uint8(creation[creation.length - i])), uint256(uint8(runtime[runtime.length - i])));
        }
    }
}
