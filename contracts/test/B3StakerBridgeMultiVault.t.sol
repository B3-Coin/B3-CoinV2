// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {TestBase} from "./TestBase.sol";
import {B3Types, IB3FinalityProver} from "../src/IB3FinalityProver.sol";
import {B3BridgeAssetId} from "../src/B3BridgeAssetId.sol";
import {B3FinalityVerifier} from "../src/B3FinalityVerifier.sol";
import {B3StakerBridge} from "../src/B3StakerBridge.sol";

contract MultiVaultFinalityProver is IB3FinalityProver {
    function verify(bytes32, B3Types.FinalizedBlock calldata, bytes32, B3Types.SetHeader calldata, bytes calldata)
        external
        pure
        returns (bool)
    {
        return true;
    }
}

/// USDT-shaped token: transfer functions deliberately return no value.
contract MultiVaultUSDT {
    mapping(address => uint256) public balanceOf;
    mapping(address => mapping(address => uint256)) public allowance;

    function decimals() external pure returns (uint8) {
        return 6;
    }

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

    function transferFrom(address sender, address recipient, uint256 amount) external {
        allowance[sender][msg.sender] -= amount;
        balanceOf[sender] -= amount;
        balanceOf[recipient] += amount;
    }
}

/// A conventional six-decimal ERC-20 representing a possible future USDC
/// vault. This fixture deliberately does not claim support for WETH/WBTC:
/// their non-six decimal conversion needs a later vault version.
contract MultiVaultUSDC6 {
    mapping(address => uint256) public balanceOf;
    mapping(address => mapping(address => uint256)) public allowance;

    function decimals() external pure returns (uint8) {
        return 6;
    }

    function mint(address account, uint256 amount) external {
        balanceOf[account] += amount;
    }

    function approve(address spender, uint256 amount) external returns (bool) {
        allowance[msg.sender][spender] = amount;
        return true;
    }

    function transfer(address recipient, uint256 amount) external returns (bool) {
        balanceOf[msg.sender] -= amount;
        balanceOf[recipient] += amount;
        return true;
    }

    function transferFrom(address sender, address recipient, uint256 amount) external returns (bool) {
        allowance[sender][msg.sender] -= amount;
        balanceOf[sender] -= amount;
        balanceOf[recipient] += amount;
        return true;
    }
}

contract B3StakerBridgeMultiVaultTest is TestBase {
    bytes32 private constant DOMAIN = keccak256("multi-vault-chain-domain");
    uint64 private constant MODERN_HEIGHT = 100;
    uint64 private constant BRIDGE_HEIGHT = 200;
    uint256 private constant MAX_DEPOSIT_RAW = 1_000_000;

    function test_SharedVerifierKeepsTwoSixDecimalVaultsAndProofsIsolated() public {
        MultiVaultFinalityProver prover = new MultiVaultFinalityProver();
        B3Types.SetHeader memory genesis = _set(0, 4, 1_000, 1);
        B3FinalityVerifier verifier = _verifier(prover, genesis);
        MultiVaultUSDT usdt = new MultiVaultUSDT();
        MultiVaultUSDC6 usdc = new MultiVaultUSDC6();
        B3StakerBridge usdtVault =
            new B3StakerBridge(verifier, address(verifier).codehash, DOMAIN, address(usdt), MAX_DEPOSIT_RAW);
        B3StakerBridge usdcVault =
            new B3StakerBridge(verifier, address(verifier).codehash, DOMAIN, address(usdc), MAX_DEPOSIT_RAW);

        assertEq(uint256(uint160(address(usdtVault.verifier()))), uint256(uint160(address(verifier))));
        assertEq(uint256(uint160(address(usdcVault.verifier()))), uint256(uint160(address(verifier))));
        assertEq(uint256(usdt.decimals()), 6);
        assertEq(uint256(usdc.decimals()), 6);
        assertEq(
            usdtVault.B3_ASSET_ID(),
            B3BridgeAssetId.compute(DOMAIN, uint64(block.chainid), address(usdtVault), address(usdt))
        );
        assertEq(
            usdcVault.B3_ASSET_ID(),
            B3BridgeAssetId.compute(DOMAIN, uint64(block.chainid), address(usdcVault), address(usdc))
        );
        assertTrue(usdtVault.B3_ASSET_ID() != usdcVault.B3_ASSET_ID());

        B3Types.SetHeader memory successor = _set(1, 4, 1_000, 2);
        verifier.submitCertificate(
            _finalized(BRIDGE_HEIGHT, _emptyWithdrawalRoot(), _setHash(successor), 0), successor, ""
        );
        assertTrue(verifier.depositViable());

        usdt.mint(address(this), 600);
        usdc.mint(address(this), 900);
        usdt.approve(address(usdtVault), 600);
        usdc.approve(address(usdcVault), 900);
        usdtVault.deposit(600, _recipient(0xA1));
        usdcVault.deposit(900, _recipient(0xB2));

        assertEq(usdtVault.locked(), 600);
        assertEq(usdcVault.locked(), 900);
        assertEq(usdt.balanceOf(address(usdtVault)), 600);
        assertEq(usdt.balanceOf(address(usdcVault)), 0);
        assertEq(usdc.balanceOf(address(usdtVault)), 0);
        assertEq(usdc.balanceOf(address(usdcVault)), 900);

        B3StakerBridge.Withdrawal memory usdtWithdrawal = B3StakerBridge.Withdrawal({
            withdrawalId: 0, recipient: address(0xA11CE), amount: 250, b3Height: BRIDGE_HEIGHT
        });
        B3StakerBridge.Withdrawal memory usdcWithdrawal = B3StakerBridge.Withdrawal({
            withdrawalId: 1, recipient: address(0xB0B), amount: 400, b3Height: BRIDGE_HEIGHT
        });
        (bytes32 root, bytes32[32] memory usdtPath, bytes32[32] memory usdcPath) =
            _twoLeafRoot(usdtVault, usdtWithdrawal, usdcVault, usdcWithdrawal);
        verifier.submitCertificate(_finalized(BRIDGE_HEIGHT + 10, root, _setHash(successor), 0), successor, "");

        // A proof for the USDT leaf cannot be replayed against another vault:
        // that vault derives a different AssetId and commits its own token.
        vm.expectRevert(B3StakerBridge.BadWithdrawalProof.selector);
        usdcVault.release(usdtWithdrawal, usdtPath);
        assertEq(usdcVault.locked(), 900);
        assertFalse(usdcVault.released(0));

        usdtVault.release(usdtWithdrawal, usdtPath);
        usdcVault.release(usdcWithdrawal, usdcPath);
        assertEq(usdt.balanceOf(address(0xA11CE)), 250);
        assertEq(usdc.balanceOf(address(0xB0B)), 400);
        assertEq(usdtVault.locked(), 350);
        assertEq(usdcVault.locked(), 500);
        assertTrue(usdtVault.released(0));
        assertTrue(usdcVault.released(1));
    }

    function _verifier(MultiVaultFinalityProver prover, B3Types.SetHeader memory genesis)
        private
        returns (B3FinalityVerifier verifier)
    {
        B3Types.SetHeader memory bootstrap = _set(0, 4, 4, 0x44);
        B3FinalityVerifier.DeploymentConfig memory config = B3FinalityVerifier.DeploymentConfig({
            modernStartHeight: MODERN_HEIGHT,
            bridgeActivationHeight: BRIDGE_HEIGHT,
            minBridgeValidators: 4,
            maxBridgeValidators: 64,
            minBridgeTotalWeight: 900,
            minEpochDuration: 1 days,
            maxEpochLag: 30 days,
            maxCertificateAge: 1 days,
            minDepositExitWindow: 7 days,
            bootstrapDeadline: block.timestamp + 7 days
        });
        verifier =
            new B3FinalityVerifier(DOMAIN, bootstrap, _setHash(bootstrap), prover, address(prover).codehash, config);
        verifier.initialize(_finalized(MODERN_HEIGHT - 1, bytes32(0), _setHash(genesis), 0), genesis, "");
    }

    function _set(uint64 epoch, uint32 validators, uint64 totalWeight, uint8 seed)
        private
        pure
        returns (B3Types.SetHeader memory header)
    {
        bytes memory aggregatePubkey = new bytes(48);
        aggregatePubkey[0] = bytes1(uint8(0x80 | (seed & 0x1f)));
        aggregatePubkey[47] = bytes1(seed);
        header = B3Types.SetHeader({
            epoch: epoch,
            rulesetVersion: 1,
            validatorCount: validators,
            totalWeight: totalWeight,
            quorumWeight: uint64((uint256(totalWeight) * 2) / 3 + 1),
            aggregatePubkey: aggregatePubkey,
            membersRoot: keccak256(abi.encodePacked("multi-vault-members", seed))
        });
    }

    function _setHash(B3Types.SetHeader memory header) private pure returns (bytes32) {
        return B3Types.hashSetHeader(header);
    }

    function _finalized(uint64 height, bytes32 withdrawalRoot, bytes32 successorHash, uint64 epoch)
        private
        pure
        returns (B3Types.FinalizedBlock memory)
    {
        return B3Types.FinalizedBlock({
            height: height,
            blockHash: keccak256(abi.encodePacked("multi-vault-block", height)),
            withdrawalRoot: withdrawalRoot,
            validatorSetHash: successorHash,
            epoch: epoch
        });
    }

    function _recipient(uint160 keyHash) private pure returns (bytes32) {
        return bytes32((uint256(1) << 160) | uint256(keyHash));
    }

    function _emptyWithdrawalRoot() private pure returns (bytes32 root) {
        for (uint256 level = 0; level < 32; ++level) {
            root = keccak256(abi.encodePacked(root, root));
        }
    }

    function _twoLeafRoot(
        B3StakerBridge firstVault,
        B3StakerBridge.Withdrawal memory firstWithdrawal,
        B3StakerBridge secondVault,
        B3StakerBridge.Withdrawal memory secondWithdrawal
    ) private view returns (bytes32 root, bytes32[32] memory firstPath, bytes32[32] memory secondPath) {
        bytes32 firstLeaf = firstVault.withdrawalLeaf(firstWithdrawal);
        bytes32 secondLeaf = secondVault.withdrawalLeaf(secondWithdrawal);
        firstPath[0] = secondLeaf;
        secondPath[0] = firstLeaf;
        root = keccak256(abi.encodePacked(firstLeaf, secondLeaf));

        bytes32 zero;
        for (uint256 level = 1; level < 32; ++level) {
            zero = keccak256(abi.encodePacked(zero, zero));
            firstPath[level] = zero;
            secondPath[level] = zero;
            root = keccak256(abi.encodePacked(root, zero));
        }
    }
}
