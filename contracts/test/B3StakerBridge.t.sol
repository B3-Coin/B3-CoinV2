// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {TestBase} from "./TestBase.sol";
import {B3Types, IB3FinalityProver} from "../src/IB3FinalityProver.sol";
import {B3FinalityVerifier} from "../src/B3FinalityVerifier.sol";
import {B3StakerBridge} from "../src/B3StakerBridge.sol";
import {BlsCertificateProver} from "../src/BlsCertificateProver.sol";

contract MockFinalityProver is IB3FinalityProver {
    bool public result = true;

    function setResult(bool value) external {
        result = value;
    }

    function verify(
        bytes32,
        B3Types.FinalizedBlock calldata,
        bytes32,
        B3Types.SetHeader calldata,
        bytes calldata
    ) external view returns (bool) {
        return result;
    }
}

/// USDT-shaped mock: approve/transfer/transferFrom return no value.
contract MockBridgeUSDT {
    mapping(address => uint256) public balanceOf;
    mapping(address => mapping(address => uint256)) public allowance;

    function mint(address account, uint256 amount) external {
        balanceOf[account] += amount;
    }

    function approve(address spender, uint256 amount) external {
        allowance[msg.sender][spender] = amount;
    }

    function transfer(address recipient, uint256 amount) external {
        balanceOf[msg.sender] -= amount;
        balanceOf[recipient] += amount;
    }

    function transferFrom(address sender, address recipient, uint256 amount)
        external
    {
        allowance[sender][msg.sender] -= amount;
        balanceOf[sender] -= amount;
        balanceOf[recipient] += amount;
    }
}

contract B3StakerBridgeTest is TestBase {
    bytes32 private constant DOMAIN = keccak256("fixture-chain-domain");
    bytes32 private constant ASSET = keccak256("fixture-busd-asset");
    uint64 private constant M = 100;
    uint64 private constant BRIDGE_HEIGHT = 200;
    uint32 private constant MIN_VALIDATORS = 4;
    uint64 private constant MIN_WEIGHT = 900;

    MockFinalityProver private mockProver;
    MockBridgeUSDT private token;

    function setUp() public {
        mockProver = new MockFinalityProver();
        token = new MockBridgeUSDT();
    }

    function test_BootstrapPinsOnlySuppliedPublicFixture() public {
        B3Types.SetHeader memory genesis = _set(0, 4, 1_000, 1);
        B3FinalityVerifier verifier = _verifier(genesis);

        assertEq(verifier.CHAIN_DOMAIN(), DOMAIN);
        assertEq(verifier.GENESIS_SET_HASH(), _setHash(genesis));
        assertEq(verifier.PROVER_CODE_HASH(), address(mockProver).codehash);
        assertEq(verifier.MODERN_START_HEIGHT(), M);
        assertEq(verifier.BRIDGE_ACTIVATION_HEIGHT(), BRIDGE_HEIGHT);
        assertEq(verifier.MIN_BRIDGE_VALIDATORS(), MIN_VALIDATORS);
        assertEq(verifier.MIN_BRIDGE_TOTAL_WEIGHT(), MIN_WEIGHT);
        assertEq(verifier.latestFinalizedHeight(), M - 1);
        assertEq(verifier.latestWithdrawalRoot(), bytes32(0));
    }

    function test_BootstrapRejectsUnpinnedSnapshotButTracksWeakSet() public {
        B3Types.SetHeader memory genesis = _set(0, 4, 1_000, 1);

        vm.expectRevert(B3FinalityVerifier.BadGenesisSet.selector);
        new B3FinalityVerifier(
            DOMAIN,
            genesis,
            bytes32(uint256(0xBAD)),
            mockProver,
            address(mockProver).codehash,
            M,
            BRIDGE_HEIGHT,
            MIN_VALIDATORS,
            MIN_WEIGHT,
            30 days
        );

        // Chain lineage may begin with two validators. That public set is
        // tracked, while bridgeReady remains false until a qualifying set
        // actually signs a post-activation certificate.
        genesis = _set(0, 2, MIN_WEIGHT - 1, 9);
        B3FinalityVerifier verifier = _verifier(genesis);
        assertEq(verifier.currentEpoch(), 0);
        assertFalse(verifier.bridgeReady());
    }

    function test_BootstrapRejectsUnpinnedProverRuntime() public {
        B3Types.SetHeader memory genesis = _set(0, 4, 1_000, 1);
        vm.expectRevert(B3FinalityVerifier.BadProver.selector);
        new B3FinalityVerifier(
            DOMAIN,
            genesis,
            _setHash(genesis),
            mockProver,
            bytes32(uint256(1)),
            M,
            BRIDGE_HEIGHT,
            MIN_VALIDATORS,
            MIN_WEIGHT,
            30 days
        );
    }

    function test_NonzeroWithdrawalRootIsDormantBeforeBridgeHeight() public {
        B3Types.SetHeader memory genesis = _set(0, 4, 1_000, 1);
        B3FinalityVerifier verifier = _verifier(genesis);
        B3Types.SetHeader memory successor = _set(1, 4, 1_000, 2);
        B3Types.FinalizedBlock memory finalizedBlock = _finalized(
            M,
            bytes32(uint256(1)),
            _setHash(successor),
            0
        );

        vm.expectRevert(B3FinalityVerifier.PrematureWithdrawalRoot.selector);
        verifier.submitCertificate(finalizedBlock, successor, "");
    }

    function test_WeakSetTracksLineageButCannotTakeBridgeAuthority() public {
        B3Types.SetHeader memory genesis = _set(0, 2, MIN_WEIGHT - 1, 1);
        B3FinalityVerifier verifier = _verifier(genesis);
        B3Types.SetHeader memory successor = _set(1, 4, 1_000, 2);
        B3Types.FinalizedBlock memory finalizedBlock = _finalized(
            M,
            bytes32(0),
            _setHash(successor),
            0
        );

        verifier.submitCertificate(finalizedBlock, successor, "");
        assertEq(verifier.latestFinalizedHeight(), M);
        assertFalse(verifier.bridgeReady());

        // The qualifying successor becomes authoritative only when it signs
        // its own epoch-1 certificate.
        B3Types.SetHeader memory successor2 = _set(2, 4, 1_000, 3);
        verifier.submitCertificate(
            _finalized(BRIDGE_HEIGHT, bytes32(uint256(7)), _setHash(successor2), 1),
            successor2,
            ""
        );
        assertTrue(verifier.bridgeReady());
        assertEq(verifier.latestBridgeWithdrawalRoot(), bytes32(uint256(7)));
    }

    function test_FinalizedBurnProofReleasesCanonicalTokenOnce() public {
        B3Types.SetHeader memory genesis = _set(0, 4, 1_000, 1);
        B3FinalityVerifier verifier = _verifier(genesis);
        B3StakerBridge bridge = new B3StakerBridge(
            verifier,
            address(verifier).codehash,
            ASSET,
            address(token)
        );

        B3StakerBridge.Withdrawal memory withdrawal = B3StakerBridge.Withdrawal({
            withdrawalId: 0,
            recipient: address(0xBEEF),
            amount: 250_000,
            b3Height: BRIDGE_HEIGHT
        });
        B3Types.SetHeader memory successor = _set(1, 4, 1_000, 2);
        // A threshold-qualified post-activation certificate opens deposits.
        verifier.submitCertificate(
            _finalized(BRIDGE_HEIGHT, bytes32(0), _setHash(successor), 0),
            successor,
            ""
        );
        token.mint(address(this), 1_000_000);
        token.approve(address(bridge), 1_000_000);
        bridge.deposit(1_000_000, bytes32(uint256(0xB3)));
        assertEq(bridge.locked(), 1_000_000);

        (bytes32 root, bytes32[32] memory path) = _withdrawalRoot(bridge, withdrawal);
        verifier.submitCertificate(
            _finalized(BRIDGE_HEIGHT + 10, root, _setHash(successor), 0),
            successor,
            ""
        );

        bridge.release(withdrawal, path);
        assertEq(token.balanceOf(address(0xBEEF)), 250_000);
        assertEq(bridge.locked(), 750_000);
        assertTrue(bridge.released(0));

        vm.expectRevert(B3StakerBridge.AlreadyReleased.selector);
        bridge.release(withdrawal, path);
    }

    function test_BridgeStaysClosedWithoutFinalizedRoot() public {
        B3FinalityVerifier verifier = _verifier(_set(0, 4, 1_000, 1));
        B3StakerBridge bridge = new B3StakerBridge(
            verifier,
            address(verifier).codehash,
            ASSET,
            address(token)
        );
        B3StakerBridge.Withdrawal memory withdrawal = B3StakerBridge.Withdrawal({
            withdrawalId: 0,
            recipient: address(0xBEEF),
            amount: 1,
            b3Height: BRIDGE_HEIGHT
        });
        (, bytes32[32] memory path) = _withdrawalRoot(bridge, withdrawal);

        vm.expectRevert(B3StakerBridge.BridgeNotReady.selector);
        bridge.release(withdrawal, path);

        token.mint(address(this), 10);
        token.approve(address(bridge), 10);
        vm.expectRevert(B3StakerBridge.BridgeNotReady.selector);
        bridge.deposit(10, bytes32(uint256(1)));
    }

    function test_FirstCertificateHasNoEarlyDeployClockTrap() public {
        B3FinalityVerifier verifier = _verifier(_set(0, 4, 1_000, 1));
        vm.warp(block.timestamp + 90 days);
        B3Types.SetHeader memory successor = _set(1, 4, 1_000, 2);
        verifier.submitCertificate(
            _finalized(M, bytes32(0), _setHash(successor), 0),
            successor,
            ""
        );
        assertEq(verifier.latestFinalizedHeight(), M);
    }

    function test_StaleCurrentEpochCannotBypassLagStop() public {
        B3FinalityVerifier verifier = _verifier(_set(0, 4, 1_000, 1));
        B3Types.SetHeader memory successor = _set(1, 4, 1_000, 2);
        verifier.submitCertificate(
            _finalized(M, bytes32(0), _setHash(successor), 0),
            successor,
            ""
        );
        vm.warp(block.timestamp + 30 days + 1);
        vm.expectRevert(B3FinalityVerifier.EpochLagExceeded.selector);
        verifier.submitCertificate(
            _finalized(M + 10, bytes32(0), _setHash(successor), 0),
            successor,
            ""
        );
    }

    function test_StaleVerifierClosesBridgeReadiness() public {
        B3FinalityVerifier verifier = _verifier(_set(0, 4, 1_000, 1));
        B3StakerBridge bridge = new B3StakerBridge(
            verifier,
            address(verifier).codehash,
            ASSET,
            address(token)
        );
        B3Types.SetHeader memory successor = _set(1, 4, 1_000, 2);
        verifier.submitCertificate(
            _finalized(BRIDGE_HEIGHT, bytes32(uint256(7)), _setHash(successor), 0),
            successor,
            ""
        );
        assertTrue(verifier.bridgeReady());

        vm.warp(block.timestamp + 30 days);
        assertTrue(verifier.bridgeReady());
        vm.warp(block.timestamp + 1);
        assertFalse(verifier.bridgeReady());

        vm.expectRevert(B3StakerBridge.BridgeNotReady.selector);
        bridge.deposit(1, bytes32(uint256(1)));
        B3StakerBridge.Withdrawal memory withdrawal =
            B3StakerBridge.Withdrawal({
                withdrawalId: 0,
                recipient: address(0xBEEF),
                amount: 1,
                b3Height: BRIDGE_HEIGHT
            });
        bytes32[32] memory path;
        vm.expectRevert(B3StakerBridge.BridgeNotReady.selector);
        bridge.release(withdrawal, path);

        vm.expectRevert(B3FinalityVerifier.EpochLagExceeded.selector);
        verifier.submitCertificate(
            _finalized(BRIDGE_HEIGHT + 10, bytes32(uint256(8)),
                       _setHash(successor), 0),
            successor,
            ""
        );
    }

    function test_OrderedValidatorPathAndGeneratorArePinned() public {
        BlsCertificateProver prover = new BlsCertificateProver();
        bytes memory pubkey = _publicKey(7);
        bytes32[13] memory path;
        for (uint256 i = 0; i < path.length; ++i) {
            path[i] = keccak256(abi.encodePacked("sibling", i));
        }

        uint32 index = 5;
        bytes32 expected = keccak256(abi.encodePacked(index, pubkey, uint64(77)));
        for (uint256 i = 0; i < path.length; ++i) {
            expected = ((uint256(index) >> i) & 1) == 0
                ? keccak256(abi.encodePacked(expected, path[i]))
                : keccak256(abi.encodePacked(path[i], expected));
        }
        assertEq(prover.validatorMemberRoot(index, pubkey, 77, path), expected);
        assertEq(prover.negativeGenerator().length, 128);
    }

    function test_ProverRequiresHeadcountAsWellAsWeightQuorum() public {
        BlsCertificateProver prover = new BlsCertificateProver();
        assertEq(prover.headcountQuorum(2), 2);
        assertEq(prover.headcountQuorum(4), 3);
        assertEq(prover.headcountQuorum(5), 4);

        B3Types.SetHeader memory set = _set(0, 4, 1_000, 1);
        B3Types.FinalizedBlock memory finalizedBlock;
        bytes memory bitmap = hex"01"; // one signer, even if it held >2/3 weight
        bytes memory signature = new bytes(256);
        signature[0] = bytes1(uint8(1));
        bytes memory aggregatePubkey = new bytes(128);
        BlsCertificateProver.Absent[] memory absent =
            new BlsCertificateProver.Absent[](3);
        bytes memory proof = abi.encode(
            bitmap,
            signature,
            aggregatePubkey,
            absent
        );
        assertFalse(
            prover.verify(DOMAIN, finalizedBlock, _setHash(set), set, proof)
        );
    }

    function test_WithdrawalLeafMatchesB3CrossLanguageVector() public pure {
        bytes32 asset =
            hex"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
        address originToken =
            address(bytes20(hex"202122232425262728292a2b2c2d2e2f30313233"));
        address recipient =
            address(bytes20(hex"404142434445464748494a4b4c4d4e4f50515253"));
        bytes32 leaf = keccak256(
            abi.encodePacked(
                uint64(0),
                uint64(1),
                asset,
                originToken,
                recipient,
                uint256(1_000_000),
                uint64(815_000)
            )
        );
        require(
            leaf ==
                hex"f96ee37321b191d9ba3e573fd7739ab8a163033824a1c534045bd168c3c88b44",
            "B3 withdrawal leaf vector"
        );
    }

    function test_ProverRejectsWrongSetHashBeforeDecodingWitness() public {
        BlsCertificateProver prover = new BlsCertificateProver();
        B3Types.SetHeader memory set = _set(0, 4, 1_000, 1);
        B3Types.FinalizedBlock memory finalizedBlock;
        assertFalse(
            prover.verify(
                DOMAIN,
                finalizedBlock,
                bytes32(uint256(0xBAD)),
                set,
                ""
            )
        );
    }

    function _verifier(B3Types.SetHeader memory genesis)
        private
        returns (B3FinalityVerifier)
    {
        return new B3FinalityVerifier(
            DOMAIN,
            genesis,
            _setHash(genesis),
            mockProver,
            address(mockProver).codehash,
            M,
            BRIDGE_HEIGHT,
            MIN_VALIDATORS,
            MIN_WEIGHT,
            30 days
        );
    }

    function _set(
        uint64 epoch,
        uint32 validators,
        uint64 totalWeight,
        uint8 seed
    ) private pure returns (B3Types.SetHeader memory header) {
        header.epoch = epoch;
        header.rulesetVersion = 1;
        header.validatorCount = validators;
        header.totalWeight = totalWeight;
        header.quorumWeight = uint64((uint256(totalWeight) * 2) / 3 + 1);
        header.aggregatePubkey = _publicKey(seed);
        header.membersRoot = keccak256(abi.encodePacked("fixture-members", seed));
    }

    function _publicKey(uint8 seed) private pure returns (bytes memory key) {
        key = new bytes(48);
        key[0] = bytes1(uint8(0x80 | (seed & 0x1f)));
        key[47] = bytes1(seed);
    }

    function _setHash(B3Types.SetHeader memory header)
        private
        pure
        returns (bytes32)
    {
        return keccak256(
            abi.encodePacked(
                header.epoch,
                header.rulesetVersion,
                header.validatorCount,
                header.totalWeight,
                header.quorumWeight,
                header.aggregatePubkey,
                header.membersRoot
            )
        );
    }

    function _finalized(
        uint64 height,
        bytes32 withdrawalRoot,
        bytes32 successorHash,
        uint64 epoch
    ) private pure returns (B3Types.FinalizedBlock memory) {
        return B3Types.FinalizedBlock({
            height: height,
            blockHash: keccak256(abi.encodePacked("fixture-block", height)),
            withdrawalRoot: withdrawalRoot,
            validatorSetHash: successorHash,
            epoch: epoch
        });
    }

    function _withdrawalRoot(
        B3StakerBridge bridge,
        B3StakerBridge.Withdrawal memory withdrawal
    ) private view returns (bytes32 node, bytes32[32] memory path) {
        node = bridge.withdrawalLeaf(withdrawal);
        bytes32 zero;
        for (uint256 level = 0; level < path.length; ++level) {
            path[level] = zero;
            node = ((uint256(withdrawal.withdrawalId) >> level) & 1) == 0
                ? keccak256(abi.encodePacked(node, zero))
                : keccak256(abi.encodePacked(zero, node));
            zero = keccak256(abi.encodePacked(zero, zero));
        }
    }
}
