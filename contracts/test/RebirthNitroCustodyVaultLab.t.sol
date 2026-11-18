// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {RebirthCustodyVault} from "../custody/RebirthCustodyVault.sol";
import {RebirthNitroCustodyVaultLab} from "../labnitro/RebirthNitroCustodyVaultLab.sol";
import {RebirthNitroLabToken} from "../labnitro/RebirthNitroLabToken.sol";
import {CustodyTestToken} from "./custody/RebirthCustodyVault.t.sol";

interface VmOwnedNitroLab {
    function chainId(uint256 chain) external;
    function expectRevert(bytes4 selector) external;
    function prank(address sender) external;
}

/// Local constructor/guard coverage only. The inherited vault's full custody
/// tests remain authoritative for its unchanged accounting/authorization code.
contract RebirthNitroCustodyVaultLabTest {
    VmOwnedNitroLab private constant vm = VmOwnedNitroLab(address(uint160(uint256(keccak256("hevm cheat code")))));
    CustodyTestToken private token;
    bytes32 private constant ASSET = keccak256("TEST owned Nitro exact proxy-bound quote");
    bytes32 private constant INSTANCE = keccak256("TEST owned Nitro deployment");
    bytes32 private constant ACCOUNT = keccak256("TEST lab account");

    function setUp() public {
        vm.chainId(412346);
        token = new CustodyTestToken();
    }

    function _deploy(uint256 chain, uint64 dispute, uint64 rotation) private returns (RebirthNitroCustodyVaultLab) {
        address[4] memory roster = [address(1), address(2), address(3), address(4)];
        return new RebirthNitroCustodyVaultLab(address(token), ASSET, INSTANCE, roster,
            RebirthCustodyVault.Policy(chain, dispute, rotation, 3600));
    }

    function test_lab_identity_and_inherited_exact_deposit() public {
        RebirthNitroCustodyVaultLab vault = _deploy(412346, 300, 7 days);
        require(vault.LAB_ONLY() && vault.LAB_CHAIN_ID() == 412346 && vault.DEPLOYMENT_CHAIN_ID() == 412346, "lab identity");
        require(vault.DISPUTE_SECONDS() == 300 && vault.ROTATION_SECONDS() == 7 days, "inherited policy");
        require(vault.rosterEpoch() == 0, "initial roster epoch");
        for (uint256 i; i < 4; ++i) require(vault.operators(i) == address(uint160(i + 1)), "initial operator order");
        require(vault.Q_ASSET_ID() == ASSET && vault.INSTANCE_ID() == INSTANCE && vault.TOKEN_CODE_HASH() == address(token).codehash, "bound identity");
        token.mint(address(this), 1_000_000);
        token.approve(address(vault), 1_000_000);
        vault.deposit(1_000_000, ACCOUNT);
        require(vault.locked() == 1_000_000 && token.balanceOf(address(vault)) == 1_000_000 && vault.nextDepositId() == 1, "exact deposit");
    }

    function test_lab_rejects_old_regtest_and_live_chain_deployment() public {
        vm.chainId(31337);
        vm.expectRevert(RebirthNitroCustodyVaultLab.NitroLabChainRequired.selector);
        _deploy(31337, 300, 7 days);
        vm.chainId(42161);
        vm.expectRevert(RebirthNitroCustodyVaultLab.NitroLabChainRequired.selector);
        _deploy(42161, 300, 7 days);
        vm.chainId(1);
        vm.expectRevert(RebirthNitroCustodyVaultLab.NitroLabChainRequired.selector);
        _deploy(1, 300, 7 days);
    }

    function test_lab_rejects_policy_chain_substitution() public {
        vm.expectRevert(RebirthCustodyVault.WrongChain.selector);
        _deploy(31337, 300, 7 days);
    }

    function test_lab_keeps_the_inherited_300_second_floor() public {
        vm.expectRevert(RebirthCustodyVault.BadConfiguration.selector);
        _deploy(412346, 299, 7 days);
    }

    function test_lab_keeps_the_inherited_seven_day_rotation_floor() public {
        vm.expectRevert(RebirthCustodyVault.BadConfiguration.selector);
        _deploy(412346, 300, 7 days - 1);
    }

    function test_lab_runtime_chain_change_refuses_inherited_entrypoints() public {
        RebirthNitroCustodyVaultLab vault = _deploy(412346, 300, 7 days);
        vm.chainId(31337);
        vm.expectRevert(RebirthCustodyVault.WrongChain.selector);
        vault.deposit(1, ACCOUNT);
    }

    function test_synthetic_base_token_has_exact_transfers_and_owner_funding() public {
        RebirthNitroLabToken base = new RebirthNitroLabToken("Owned Nitro synthetic TOKENA", "LABA");
        require(base.LAB_ONLY() && base.LAB_CHAIN_ID() == 412346 && base.decimals() == 6 && base.minter() == address(this), "base lab identity");
        base.mint(address(this), 1_000_000);
        base.transfer(address(7), 200_000);
        base.approve(address(8), 300_000);
        vm.prank(address(8)); base.transferFrom(address(this), address(7), 300_000);
        require(base.totalSupply() == 1_000_000 && base.balanceOf(address(this)) == 500_000 && base.balanceOf(address(7)) == 500_000, "base exact conservation");
        require(base.allowance(address(this), address(8)) == 0, "base exact allowance");
    }

    function test_synthetic_base_token_rejects_foreign_chains() public {
        RebirthNitroLabToken base = new RebirthNitroLabToken("Owned Nitro synthetic TOKENA", "LABA");
        vm.chainId(31337);
        vm.expectRevert(RebirthNitroLabToken.NotNitroLab.selector);
        new RebirthNitroLabToken("Wrong chain", "BAD");
        vm.chainId(42161);
        vm.expectRevert(RebirthNitroLabToken.NotNitroLab.selector);
        base.mint(address(this), 1);
        vm.expectRevert(RebirthNitroLabToken.NotNitroLab.selector);
        base.approve(address(8), 1);
    }

    function test_synthetic_base_token_does_not_allow_arbitrary_minting() public {
        RebirthNitroLabToken base = new RebirthNitroLabToken("Owned Nitro synthetic TOKENA", "LABA");
        vm.expectRevert(RebirthNitroLabToken.NotMinter.selector);
        vm.prank(address(99)); base.mint(address(99), 1_000_000);
        require(base.totalSupply() == 0 && base.balanceOf(address(99)) == 0, "unauthorized mint changed supply");
    }
}
