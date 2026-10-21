// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {RebirthRegtestToken} from "../../regtest/RebirthRegtestToken.sol";
import {RebirthRegtestVault} from "../../regtest/RebirthRegtestVault.sol";

interface VmRebirthCustody {
    function chainId(uint256 chain) external;
    function warp(uint256 timestamp) external;
    function addr(uint256 privateKey) external returns (address);
    function sign(uint256 privateKey, bytes32 digest) external returns (uint8 v, bytes32 r, bytes32 s);
    function expectRevert(bytes4 selector) external;
    function prank(address sender) external;
    function expectEmit(bool topic1, bool topic2, bool topic3, bool data, address emitter) external;
}

/// Fresh local-only tests; no RPC, fork, deployed token or external keys.
contract RebirthRegtestVaultTest {
    VmRebirthCustody private constant vm = VmRebirthCustody(address(uint160(uint256(keccak256("hevm cheat code")))));
    bytes32 private constant Q = 0x6439dd037ac1af8e258b4177319230f77f127c3fcf5ec6436aba005bb25bd7ed;
    bytes32 private constant ACCOUNT = keccak256("TEST rebirth account");
    bytes32 private constant INSTANCE = keccak256("TEST rebirth custody instance");
    address private constant RECIPIENT = address(0x1234);
    uint256[4] private keys;
    address[4] private roster;
    RebirthRegtestToken private token;
    RebirthRegtestVault private vault;

    event Deposit(uint64 indexed depositId, bytes32 indexed account, bytes32 indexed asset, uint256 amount, address sender);

    function setUp() public {
        vm.chainId(31337);
        vm.warp(10_000);
        for (uint256 i; i < 4; ++i) {
            keys[i] = 101 + i;
            roster[i] = vm.addr(keys[i]);
        }
        // Vault signatures and its immutable roster use ascending addresses.
        for (uint256 i; i < 4; ++i) {
            for (uint256 j = i + 1; j < 4; ++j) {
                if (roster[j] < roster[i]) {
                    (roster[i], roster[j]) = (roster[j], roster[i]);
                    (keys[i], keys[j]) = (keys[j], keys[i]);
                }
            }
        }
        token = new RebirthRegtestToken();
        vault = new RebirthRegtestVault(address(token), Q, INSTANCE, roster, 3);
        token.mint(address(this), 10_000_000);
        token.approve(address(vault), type(uint256).max);
    }

    function _assert(bool condition) private pure {
        require(condition, "custody assertion failed");
    }

    function _sign(bytes32 digest, uint256 count) private returns (bytes[] memory signatures) {
        signatures = new bytes[](count);
        for (uint256 i; i < count; ++i) {
            (uint8 v, bytes32 r, bytes32 s) = vm.sign(keys[i], digest);
            signatures[i] = abi.encodePacked(r, s, v);
        }
    }

    function _withdrawal(uint256 id, uint256 amount) private view returns (RebirthRegtestVault.Withdrawal memory) {
        return RebirthRegtestVault.Withdrawal({
            id: bytes32(id),
            account: ACCOUNT,
            asset: Q,
            amount: amount,
            recipient: RECIPIENT,
            slot: 4,
            stateRoot: keccak256("TEST committed state"),
            validUntil: uint64(block.timestamp + 100)
        });
    }

    function _request(RebirthRegtestVault.Withdrawal memory w) private {
        bytes[] memory signatures = _sign(vault.withdrawalDigest(w), 3);
        vault.requestWithdrawal(w, signatures);
    }

    function test_deposit_credits_actual_custody_and_exact_event() public {
        vm.expectEmit(true, true, true, true, address(vault));
        emit Deposit(0, ACCOUNT, Q, 1_000_000, address(this));
        vault.deposit(1_000_000, ACCOUNT);
        _assert(token.balanceOf(address(vault)) == 1_000_000);
        _assert(vault.locked() == 1_000_000 && vault.nextDepositId() == 1 && vault.pendingTotal() == 0);
        _assert(token.balanceOf(address(this)) == 9_000_000);
        // An unlogged token transfer is not creditable custody reserve.
        token.transfer(address(vault), 5);
        _assert(vault.locked() == 1_000_000 && vault.nextDepositId() == 1);
    }

    function test_three_independent_signatures_dispute_then_actual_payout() public {
        vault.deposit(1_000_000, ACCOUNT);
        RebirthRegtestVault.Withdrawal memory w = _withdrawal(1, 250_000);
        _request(w);
        (bytes32 digest, address recipient, uint256 amount, uint64 releaseAt, bool paid) = vault.withdrawals(w.id);
        _assert(digest == vault.withdrawalDigest(w) && recipient == RECIPIENT && amount == 250_000 && !paid);
        _assert(vault.pendingTotal() == 250_000 && vault.locked() == 1_000_000);
        vm.expectRevert(RebirthRegtestVault.DisputeWindowOpen.selector);
        vault.finalizeWithdrawal(w.id);
        vm.warp(releaseAt);
        vm.prank(address(0x9999)); // relay identity grants no payment authority
        vault.finalizeWithdrawal(w.id);
        _assert(token.balanceOf(RECIPIENT) == 250_000 && token.balanceOf(address(vault)) == 750_000);
        _assert(vault.pendingTotal() == 0 && vault.locked() == 750_000);
        (,,,, paid) = vault.withdrawals(w.id);
        _assert(paid);
    }

    function test_two_signatures_duplicate_signer_and_wrong_signer_refuse() public {
        vault.deposit(1_000_000, ACCOUNT);
        RebirthRegtestVault.Withdrawal memory w = _withdrawal(1, 250_000);
        bytes32 digest = vault.withdrawalDigest(w);
        bytes[] memory two = _sign(digest, 2);
        vm.expectRevert(RebirthRegtestVault.BadQuorum.selector);
        vault.requestWithdrawal(w, two);
        bytes[] memory duplicate = _sign(digest, 3);
        duplicate[1] = duplicate[0];
        vm.expectRevert(RebirthRegtestVault.BadSignature.selector);
        vault.requestWithdrawal(w, duplicate);
        bytes[] memory wrong = _sign(digest, 3);
        (uint8 v, bytes32 r, bytes32 s) = vm.sign(999_999, digest);
        wrong[2] = abi.encodePacked(r, s, v);
        vm.expectRevert(RebirthRegtestVault.BadSignature.selector);
        vault.requestWithdrawal(w, wrong);
        _assert(vault.pendingTotal() == 0 && vault.locked() == 1_000_000);
    }

    function test_all_four_signatures_are_accepted() public {
        vault.deposit(1_000_000, ACCOUNT);
        RebirthRegtestVault.Withdrawal memory w = _withdrawal(1, 250_000);
        vault.requestWithdrawal(w, _sign(vault.withdrawalDigest(w), 4));
        _assert(vault.pendingTotal() == 250_000);
    }

    function test_signature_binds_amount_recipient_account_slot_root_and_id() public {
        vault.deposit(1_000_000, ACCOUNT);
        RebirthRegtestVault.Withdrawal memory w = _withdrawal(1, 250_000);
        bytes[] memory signatures = _sign(vault.withdrawalDigest(w), 3);
        for (uint256 field; field < 6; ++field) {
            RebirthRegtestVault.Withdrawal memory wrong = _withdrawal(1, 250_000);
            if (field == 0) ++wrong.amount;
            if (field == 1) wrong.recipient = address(0x5678);
            if (field == 2) wrong.account = keccak256("TEST other account");
            if (field == 3) ++wrong.slot;
            if (field == 4) wrong.stateRoot = keccak256("TEST other state");
            if (field == 5) wrong.id = bytes32(uint256(2));
            vm.expectRevert(RebirthRegtestVault.BadSignature.selector);
            vault.requestWithdrawal(wrong, signatures);
        }
        _assert(vault.pendingTotal() == 0);
    }

    function test_signature_domain_binds_vault_and_instance() public {
        vault.deposit(1_000_000, ACCOUNT);
        RebirthRegtestVault other = new RebirthRegtestVault(address(token), Q, keccak256("TEST other instance"), roster, 3);
        token.approve(address(other), 1_000_000);
        other.deposit(1_000_000, ACCOUNT);
        RebirthRegtestVault.Withdrawal memory w = _withdrawal(1, 250_000);
        bytes[] memory signatures = _sign(vault.withdrawalDigest(w), 3);
        _assert(vault.DOMAIN_SEPARATOR() != other.DOMAIN_SEPARATOR());
        vm.expectRevert(RebirthRegtestVault.BadSignature.selector);
        other.requestWithdrawal(w, signatures);
    }

    function test_wrong_asset_zero_recipient_zero_amount_and_expiry_refuse() public {
        vault.deposit(1_000_000, ACCOUNT);
        bytes[] memory empty = new bytes[](0);
        RebirthRegtestVault.Withdrawal memory w = _withdrawal(1, 250_000);
        w.asset = keccak256("TEST wrong Q");
        vm.expectRevert(RebirthRegtestVault.InvalidWithdrawal.selector);
        vault.requestWithdrawal(w, empty);
        w = _withdrawal(1, 250_000);
        w.recipient = address(0);
        vm.expectRevert(RebirthRegtestVault.InvalidWithdrawal.selector);
        vault.requestWithdrawal(w, empty);
        w = _withdrawal(1, 0);
        vm.expectRevert(RebirthRegtestVault.InvalidWithdrawal.selector);
        vault.requestWithdrawal(w, empty);
        w = _withdrawal(1, 250_000);
        w.validUntil = uint64(block.timestamp - 1);
        vm.expectRevert(RebirthRegtestVault.ExpiredCertificate.selector);
        vault.requestWithdrawal(w, empty);
    }

    function test_replayed_queue_and_payout_never_pay_twice() public {
        vault.deposit(1_000_000, ACCOUNT);
        RebirthRegtestVault.Withdrawal memory w = _withdrawal(1, 250_000);
        bytes[] memory signatures = _sign(vault.withdrawalDigest(w), 3);
        vault.requestWithdrawal(w, signatures);
        vm.expectRevert(RebirthRegtestVault.AlreadyRequested.selector);
        vault.requestWithdrawal(w, signatures);
        vm.warp(block.timestamp + 3);
        vault.finalizeWithdrawal(w.id);
        vm.expectRevert(RebirthRegtestVault.AlreadyPaid.selector);
        vault.finalizeWithdrawal(w.id);
        vm.expectRevert(RebirthRegtestVault.AlreadyRequested.selector);
        vault.requestWithdrawal(w, signatures);
        _assert(token.balanceOf(RECIPIENT) == 250_000 && vault.locked() == 750_000);
    }

    function test_pending_reservations_prevent_overcommitted_custody() public {
        vault.deposit(1_000_000, ACCOUNT);
        _request(_withdrawal(1, 600_000));
        RebirthRegtestVault.Withdrawal memory w = _withdrawal(2, 600_000);
        bytes[] memory signatures = _sign(vault.withdrawalDigest(w), 3);
        vm.expectRevert(RebirthRegtestVault.InsufficientReserve.selector);
        vault.requestWithdrawal(w, signatures);
        _assert(vault.pendingTotal() == 600_000 && vault.locked() == 1_000_000);
    }

    function test_pause_dispute_preserves_claim_then_quorum_unpauses() public {
        vault.deposit(1_000_000, ACCOUNT);
        RebirthRegtestVault.Withdrawal memory w = _withdrawal(1, 250_000);
        _request(w);
        bytes[] memory pauseSignatures = _sign(vault.pauseDigest(true, 1), 3);
        vault.setPaused(true, 1, pauseSignatures);
        vm.warp(block.timestamp + 4);
        vm.expectRevert(RebirthRegtestVault.Paused.selector);
        vault.finalizeWithdrawal(w.id);
        vm.expectRevert(RebirthRegtestVault.Paused.selector);
        vault.deposit(1, ACCOUNT);
        bytes[] memory unpauseSignatures = _sign(vault.pauseDigest(false, 2), 3);
        vault.setPaused(false, 2, unpauseSignatures);
        vm.expectRevert(RebirthRegtestVault.BadPauseNonce.selector);
        vault.setPaused(true, 1, pauseSignatures);
        vault.finalizeWithdrawal(w.id);
        _assert(token.balanceOf(RECIPIENT) == 250_000);
    }

    function test_pause_message_cannot_be_used_as_a_withdrawal_signature() public {
        vault.deposit(1_000_000, ACCOUNT);
        RebirthRegtestVault.Withdrawal memory w = _withdrawal(1, 250_000);
        bytes[] memory signatures = _sign(vault.pauseDigest(true, 1), 3);
        vm.expectRevert(RebirthRegtestVault.BadSignature.selector);
        vault.requestWithdrawal(w, signatures);
        bytes[] memory two = _sign(vault.pauseDigest(true, 1), 2);
        vm.expectRevert(RebirthRegtestVault.BadQuorum.selector);
        vault.setPaused(true, 1, two);
    }

    function test_failed_external_transfer_keeps_pending_claim_payable() public {
        vault.deposit(1_000_000, ACCOUNT);
        RebirthRegtestVault.Withdrawal memory w = _withdrawal(1, 250_000);
        _request(w);
        vm.warp(block.timestamp + 3);
        token.setTransfersEnabled(false);
        vm.expectRevert(RebirthRegtestVault.TransferFailed.selector);
        vault.finalizeWithdrawal(w.id);
        (,,,, bool paid) = vault.withdrawals(w.id);
        _assert(!paid && vault.pendingTotal() == 250_000 && vault.locked() == 1_000_000);
        _assert(token.balanceOf(RECIPIENT) == 0);
        token.setTransfersEnabled(true);
        vault.finalizeWithdrawal(w.id);
        _assert(token.balanceOf(RECIPIENT) == 250_000 && vault.pendingTotal() == 0);
    }

    function test_accepted_claim_survives_certificate_acceptance_expiry() public {
        vault.deposit(1_000_000, ACCOUNT);
        RebirthRegtestVault.Withdrawal memory w = _withdrawal(1, 250_000);
        _request(w);
        vm.warp(uint256(w.validUntil) + 1);
        vault.finalizeWithdrawal(w.id);
        _assert(token.balanceOf(RECIPIENT) == 250_000);
    }

    function test_chain_gate_at_constructor_and_after_deployment() public {
        RebirthRegtestVault.Withdrawal memory w = _withdrawal(1, 1);
        bytes[] memory signatures = _sign(vault.withdrawalDigest(w), 3);
        vm.chainId(1);
        vm.expectRevert(RebirthRegtestToken.NotRegtest.selector);
        new RebirthRegtestToken();
        vm.expectRevert(RebirthRegtestVault.NotRegtest.selector);
        new RebirthRegtestVault(address(token), Q, INSTANCE, roster, 3);
        vm.expectRevert(RebirthRegtestVault.NotRegtest.selector);
        vault.deposit(1, ACCOUNT);
        vm.expectRevert(RebirthRegtestVault.NotRegtest.selector);
        vault.requestWithdrawal(w, signatures);
        vm.expectRevert(RebirthRegtestVault.NotRegtest.selector);
        vault.finalizeWithdrawal(w.id);
        vm.expectRevert(RebirthRegtestVault.NotRegtest.selector);
        vault.setPaused(true, 1, signatures);
        vm.expectRevert(RebirthRegtestToken.NotRegtest.selector);
        token.mint(address(this), 1);
    }

    function test_duplicate_operator_roster_and_invalid_configuration_refuse() public {
        address[4] memory duplicate = roster;
        duplicate[1] = duplicate[0];
        vm.expectRevert(RebirthRegtestVault.BadConfiguration.selector);
        new RebirthRegtestVault(address(token), Q, INSTANCE, duplicate, 3);
        vm.expectRevert(RebirthRegtestVault.BadConfiguration.selector);
        new RebirthRegtestVault(address(token), bytes32(0), INSTANCE, roster, 3);
        vm.expectRevert(RebirthRegtestVault.BadConfiguration.selector);
        new RebirthRegtestVault(address(token), Q, INSTANCE, roster, 0);
    }

    function test_high_s_and_invalid_v_signatures_refuse() public {
        vault.deposit(1_000_000, ACCOUNT);
        RebirthRegtestVault.Withdrawal memory w = _withdrawal(1, 250_000);
        bytes32 digest = vault.withdrawalDigest(w);
        bytes[] memory signatures = _sign(digest, 3);
        (uint8 v, bytes32 r,) = vm.sign(keys[0], digest);
        signatures[0] = abi.encodePacked(r, bytes32(type(uint256).max), v);
        vm.expectRevert(RebirthRegtestVault.BadSignature.selector);
        vault.requestWithdrawal(w, signatures);
        signatures = _sign(digest, 3);
        signatures[0][64] = bytes1(uint8(0));
        vm.expectRevert(RebirthRegtestVault.BadSignature.selector);
        vault.requestWithdrawal(w, signatures);
    }
}
