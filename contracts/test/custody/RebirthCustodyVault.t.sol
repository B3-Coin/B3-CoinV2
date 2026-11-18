// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {RebirthCustodyVault} from "../../custody/RebirthCustodyVault.sol";

interface VmCustodyV2 {
    function chainId(uint256 chain) external;
    function warp(uint256 timestamp) external;
    function addr(uint256 privateKey) external returns (address);
    function sign(uint256 privateKey, bytes32 digest) external returns (uint8 v, bytes32 r, bytes32 s);
    function expectRevert(bytes4 selector) external;
    function prank(address sender) external;
    function etch(address target, bytes calldata code) external;
}

/// An adversarial local token: runtime is pinned, while behavioral switches
/// exercise exact-receipt checks and failed-transfer rollback.
contract CustodyTestToken {
    uint8 public constant decimals = 6;
    mapping(address => uint256) public balanceOf;
    mapping(address => mapping(address => uint256)) public allowance;
    bool public transfersEnabled = true;
    uint256 public incomingFee;
    uint256 public outgoingFee;
    uint256 public senderFee;
    address public vault;
    function mint(address account, uint256 amount) external { balanceOf[account] += amount; }
    function setSenderFee(uint256 amount) external { senderFee = amount; }
    function burn(address account, uint256 amount) external { balanceOf[account] -= amount; }
    function approve(address spender, uint256 amount) external returns (bool) { allowance[msg.sender][spender] = amount; return true; }
    function setBehavior(address vault_, bool enabled, uint256 incoming, uint256 outgoing) external {
        vault = vault_; transfersEnabled = enabled; incomingFee = incoming; outgoingFee = outgoing;
    }
    function transfer(address recipient, uint256 amount) external returns (bool) {
        if (!transfersEnabled) return false;
        balanceOf[msg.sender] -= amount;
        balanceOf[recipient] += amount - (msg.sender == vault ? outgoingFee : 0);
        return true;
    }
    function transferFrom(address sender, address recipient, uint256 amount) external returns (bool) {
        if (!transfersEnabled) return false;
        allowance[sender][msg.sender] -= amount;
        balanceOf[sender] -= amount + senderFee;
        balanceOf[recipient] += amount - (recipient == vault ? incomingFee : 0);
        return true;
    }
}

contract RebirthCustodyVaultTest {
    VmCustodyV2 private constant vm = VmCustodyV2(address(uint160(uint256(keccak256("hevm cheat code")))));
    bytes32 private constant ASSET = keccak256("TEST stable Q");
    bytes32 private constant ACCOUNT = keccak256("TEST account");
    bytes32 private constant INSTANCE = keccak256("TEST custody deployment");
    address private constant RECIPIENT = address(0x1234);
    uint256[4] private keys;
    address[4] private roster;
    uint256[4] private nextKeys;
    address[4] private nextRoster;
    CustodyTestToken private token;
    RebirthCustodyVault private vault;

    function setUp() public {
        vm.chainId(31337);
        vm.warp(10_000);
        (keys, roster) = _roster(101);
        (nextKeys, nextRoster) = _roster(201);
        token = new CustodyTestToken();
        vault = _deploy(ASSET, INSTANCE, roster, 30, 60, 3600);
        token.setBehavior(address(vault), true, 0, 0);
        token.mint(address(this), 10_000_000);
        token.approve(address(vault), type(uint256).max);
    }
    function _roster(uint256 first) private returns (uint256[4] memory k, address[4] memory r) {
        for (uint256 i; i < 4; ++i) { k[i] = first + i; r[i] = vm.addr(k[i]); }
        for (uint256 i; i < 4; ++i) for (uint256 j = i + 1; j < 4; ++j)
            if (r[j] < r[i]) { (r[i], r[j]) = (r[j], r[i]); (k[i], k[j]) = (k[j], k[i]); }
    }
    function _deploy(bytes32 asset, bytes32 instance, address[4] memory operators,
        uint64 dispute, uint64 rotation, uint64 lifetime) private returns (RebirthCustodyVault)
    {
        return new RebirthCustodyVault(address(token), asset, instance, operators,
            RebirthCustodyVault.Policy(block.chainid, dispute, rotation, lifetime));
    }
    function _assert(bool condition) private pure { require(condition, "custody v2 assertion failed"); }
    function _sign(bytes32 digest, uint256 count, uint256[4] memory signingKeys) private returns (bytes[] memory s) {
        s = new bytes[](count);
        for (uint256 i; i < count; ++i) {
            (uint8 v, bytes32 r, bytes32 scalar) = vm.sign(signingKeys[i], digest);
            s[i] = abi.encodePacked(r, scalar, v);
        }
    }
    function _w(uint256 id, uint256 amount) private view returns (RebirthCustodyVault.Withdrawal memory) {
        return RebirthCustodyVault.Withdrawal(bytes32(id), ACCOUNT, ASSET, amount, RECIPIENT, 4,
            keccak256("TEST committed state"), uint64(block.timestamp + 100));
    }
    function _request(RebirthCustodyVault.Withdrawal memory w) private {
        vault.requestWithdrawal(w, _sign(vault.withdrawalDigest(w), 3, keys));
    }
    function _pause(bool pause, uint64 nonce, uint256[4] memory signingKeys) private {
        vault.setPaused(pause, nonce, _sign(vault.pauseDigest(pause, nonce), 3, signingKeys));
    }
    function _schedule(uint64 when, uint64 nonce) private {
        vault.scheduleRotation(nextRoster, when, nonce,
            _sign(vault.rotationDigest(nextRoster, when, nonce), 3, keys),
            _sign(vault.acceptanceDigest(nextRoster, when, nonce), 3, nextKeys));
    }
    function _invariant() private view {
        _assert(vault.pendingTotal() <= vault.locked());
        _assert(vault.locked() <= token.balanceOf(address(vault)));
        _assert(vault.locked() <= vault.MAX_ATOMS());
    }

    function test_deposit_exact_amount_and_donation_does_not_create_credit() public {
        vault.deposit(1_000_000, ACCOUNT);
        _assert(vault.nextDepositId() == 1 && vault.locked() == 1_000_000);
        token.transfer(address(vault), 99);
        _assert(vault.nextDepositId() == 1 && vault.locked() == 1_000_000);
        _invariant();
    }
    function test_queued_claim_real_payout_then_replay_refused() public {
        vault.deposit(1_000_000, ACCOUNT);
        RebirthCustodyVault.Withdrawal memory w = _w(1, 250_000);
        _request(w);
        (bytes32 digest, address recipient, uint256 amount, uint64 releaseAt, bool paid) = vault.withdrawals(w.id);
        _assert(digest == vault.withdrawalDigest(w) && recipient == RECIPIENT && amount == 250_000 && !paid);
        vm.expectRevert(RebirthCustodyVault.DisputeWindowOpen.selector);
        vault.finalizeWithdrawal(w.id);
        vm.warp(releaseAt);
        vm.prank(address(0x9999));
        vault.finalizeWithdrawal(w.id);
        _assert(token.balanceOf(RECIPIENT) == 250_000 && vault.locked() == 750_000 && vault.pendingTotal() == 0);
        vm.expectRevert(RebirthCustodyVault.AlreadyPaid.selector);
        vault.finalizeWithdrawal(w.id);
        bytes[] memory replay = _sign(vault.withdrawalDigest(w), 3, keys);
        vm.expectRevert(RebirthCustodyVault.AlreadyRequested.selector);
        vault.requestWithdrawal(w, replay);
        _invariant();
    }
    function test_two_signatures_and_duplicates_refuse_atomically() public {
        vault.deposit(1_000_000, ACCOUNT);
        RebirthCustodyVault.Withdrawal memory w = _w(1, 250_000);
        bytes32 digest = vault.withdrawalDigest(w);
        bytes[] memory s = _sign(digest, 2, keys);
        vm.expectRevert(RebirthCustodyVault.BadQuorum.selector);
        vault.requestWithdrawal(w, s);
        s = _sign(digest, 3, keys); s[1] = s[0];
        vm.expectRevert(RebirthCustodyVault.BadSignature.selector);
        vault.requestWithdrawal(w, s);
        _assert(vault.pendingTotal() == 0);
        _invariant();
    }
    function test_all_four_signatures_accepted() public {
        vault.deposit(1_000_000, ACCOUNT);
        RebirthCustodyVault.Withdrawal memory w = _w(1, 250_000);
        vault.requestWithdrawal(w, _sign(vault.withdrawalDigest(w), 4, keys));
        _assert(vault.pendingTotal() == 250_000);
    }
    function test_every_withdrawal_field_is_authenticated() public {
        vault.deposit(1_000_000, ACCOUNT);
        RebirthCustodyVault.Withdrawal memory w = _w(1, 250_000);
        bytes[] memory s = _sign(vault.withdrawalDigest(w), 3, keys);
        for (uint256 field; field < 8; ++field) {
            RebirthCustodyVault.Withdrawal memory bad = _w(1, 250_000);
            if (field == 0) ++bad.amount;
            if (field == 1) bad.recipient = address(0x5678);
            if (field == 2) bad.account = keccak256("other account");
            if (field == 3) ++bad.slot;
            if (field == 4) bad.stateRoot = keccak256("other state");
            if (field == 5) bad.id = bytes32(uint256(2));
            if (field == 6) ++bad.validUntil;
            if (field == 7) bad.asset = keccak256("other asset");
            vm.expectRevert(field == 7 ? RebirthCustodyVault.InvalidWithdrawal.selector : RebirthCustodyVault.BadSignature.selector);
            vault.requestWithdrawal(bad, s);
        }
        _assert(vault.pendingTotal() == 0);
    }
    function test_token_vault_instance_policy_and_chain_domain_bound() public {
        RebirthCustodyVault other = _deploy(ASSET, keccak256("other instance"), roster, 30, 60, 3600);
        RebirthCustodyVault policy = _deploy(ASSET, INSTANCE, roster, 31, 60, 3600);
        _assert(vault.DOMAIN_SEPARATOR() != other.DOMAIN_SEPARATOR());
        _assert(vault.DOMAIN_SEPARATOR() != policy.DOMAIN_SEPARATOR() && vault.POLICY_HASH() != policy.POLICY_HASH());
        _assert(vault.TOKEN_CODE_HASH() == address(token).codehash);
        token.approve(address(other), 1_000_000); other.deposit(1_000_000, ACCOUNT);
        RebirthCustodyVault.Withdrawal memory w = _w(1, 250_000);
        bytes[] memory s = _sign(vault.withdrawalDigest(w), 3, keys);
        vm.expectRevert(RebirthCustodyVault.BadSignature.selector);
        other.requestWithdrawal(w, s);
        vm.chainId(1);
        vm.expectRevert(RebirthCustodyVault.WrongChain.selector);
        vault.deposit(1, ACCOUNT);
        vm.expectRevert(RebirthCustodyVault.WrongChain.selector);
        vault.requestWithdrawal(w, s);
    }
    function test_replaced_token_runtime_refuses_all_signing_and_payment_paths() public {
        vault.deposit(1_000_000, ACCOUNT);
        RebirthCustodyVault.Withdrawal memory w = _w(1, 250_000);
        _request(w);
        vm.etch(address(token), hex"60006000fd");
        vm.expectRevert(RebirthCustodyVault.TokenCodeChanged.selector); vault.deposit(1, ACCOUNT);
        vm.expectRevert(RebirthCustodyVault.TokenCodeChanged.selector); vault.withdrawalDigest(w);
        vm.expectRevert(RebirthCustodyVault.TokenCodeChanged.selector); vault.finalizeWithdrawal(w.id);
    }
    function test_fee_on_incoming_transfer_refuses_no_ledger_credit() public {
        token.setBehavior(address(vault), true, 1, 0);
        vm.expectRevert(RebirthCustodyVault.NonExactTransfer.selector);
        vault.deposit(1_000_000, ACCOUNT);
        _assert(vault.locked() == 0 && vault.nextDepositId() == 0 && token.balanceOf(address(vault)) == 0);
    }
    function test_fee_charged_to_deposit_sender_refuses_no_hidden_user_charge() public {
        token.setSenderFee(1);
        vm.expectRevert(RebirthCustodyVault.NonExactTransfer.selector);
        vault.deposit(1_000_000, ACCOUNT);
        _assert(vault.locked() == 0 && vault.nextDepositId() == 0 && token.balanceOf(address(this)) == 10_000_000);
    }
    function test_fee_on_outgoing_transfer_refuses_and_retains_claim() public {
        vault.deposit(1_000_000, ACCOUNT);
        RebirthCustodyVault.Withdrawal memory w = _w(1, 250_000); _request(w);
        vm.warp(block.timestamp + 30); token.setBehavior(address(vault), true, 0, 1);
        vm.expectRevert(RebirthCustodyVault.NonExactTransfer.selector); vault.finalizeWithdrawal(w.id);
        (,,,, bool paid) = vault.withdrawals(w.id);
        _assert(!paid && vault.pendingTotal() == 250_000 && vault.locked() == 1_000_000 && token.balanceOf(RECIPIENT) == 0);
        token.setBehavior(address(vault), true, 0, 0); vault.finalizeWithdrawal(w.id);
        _assert(token.balanceOf(RECIPIENT) == 250_000); _invariant();
    }
    function test_failed_transfer_rolls_back_and_same_claim_retries() public {
        vault.deposit(1_000_000, ACCOUNT);
        RebirthCustodyVault.Withdrawal memory w = _w(1, 250_000); _request(w);
        vm.warp(block.timestamp + 30); token.setBehavior(address(vault), false, 0, 0);
        vm.expectRevert(RebirthCustodyVault.TransferFailed.selector); vault.finalizeWithdrawal(w.id);
        (,,,, bool paid) = vault.withdrawals(w.id);
        _assert(!paid && vault.pendingTotal() == 250_000 && vault.locked() == 1_000_000);
        token.setBehavior(address(vault), true, 0, 0); vault.finalizeWithdrawal(w.id); _invariant();
    }
    function test_reserve_deficit_refuses_new_credit_reservations_and_payout() public {
        vault.deposit(1_000_000, ACCOUNT);
        RebirthCustodyVault.Withdrawal memory w = _w(1, 250_000); _request(w);
        token.burn(address(vault), 1);
        vm.expectRevert(RebirthCustodyVault.InsufficientReserve.selector); vault.deposit(1, ACCOUNT);
        RebirthCustodyVault.Withdrawal memory other = _w(2, 1);
        bytes[] memory s = _sign(vault.withdrawalDigest(other), 3, keys);
        vm.expectRevert(RebirthCustodyVault.InsufficientReserve.selector); vault.requestWithdrawal(other, s);
        vm.warp(block.timestamp + 30);
        vm.expectRevert(RebirthCustodyVault.InsufficientReserve.selector); vault.finalizeWithdrawal(w.id);
        _assert(vault.pendingTotal() == 250_000 && vault.locked() == 1_000_000);
    }
    function test_pending_reservations_prevent_overcommitment() public {
        vault.deposit(1_000_000, ACCOUNT); _request(_w(1, 600_000));
        RebirthCustodyVault.Withdrawal memory w = _w(2, 600_000);
        bytes[] memory s = _sign(vault.withdrawalDigest(w), 3, keys);
        vm.expectRevert(RebirthCustodyVault.InsufficientReserve.selector); vault.requestWithdrawal(w, s);
        _assert(vault.pendingTotal() == 600_000); _invariant();
    }
    function test_expired_unaccepted_can_renew_but_accepted_survives_expiry() public {
        vault.deposit(1_000_000, ACCOUNT);
        RebirthCustodyVault.Withdrawal memory w = _w(1, 250_000);
        bytes[] memory s = _sign(vault.withdrawalDigest(w), 3, keys);
        vm.warp(uint256(w.validUntil) + 1);
        vm.expectRevert(RebirthCustodyVault.ExpiredCertificate.selector); vault.requestWithdrawal(w, s);
        w.validUntil = uint64(block.timestamp + 100); _request(w);
        vm.warp(uint256(w.validUntil) + 1); vault.finalizeWithdrawal(w.id); _invariant();
    }
    function test_unbounded_signature_lifetime_self_recipient_and_zero_fields_refuse() public {
        vault.deposit(1_000_000, ACCOUNT);
        bytes[] memory empty = new bytes[](0);
        RebirthCustodyVault.Withdrawal memory w = _w(1, 1); w.validUntil = uint64(block.timestamp + 3601);
        vm.expectRevert(RebirthCustodyVault.InvalidWithdrawal.selector); vault.requestWithdrawal(w, empty);
        w = _w(1, 1); w.recipient = address(vault);
        vm.expectRevert(RebirthCustodyVault.InvalidWithdrawal.selector); vault.requestWithdrawal(w, empty);
        w = _w(1, 0);
        vm.expectRevert(RebirthCustodyVault.InvalidWithdrawal.selector); vault.requestWithdrawal(w, empty);
        vm.expectRevert(RebirthCustodyVault.InvalidDeposit.selector); vault.deposit(0, ACCOUNT);
        vm.expectRevert(RebirthCustodyVault.InvalidDeposit.selector); vault.deposit(1, bytes32(0));
    }
    function test_pause_quorum_nonce_and_cross_action_replay_refuse() public {
        vault.deposit(1_000_000, ACCOUNT);
        RebirthCustodyVault.Withdrawal memory w = _w(1, 250_000); _request(w);
        bytes[] memory s = _sign(vault.pauseDigest(true, 1), 2, keys);
        vm.expectRevert(RebirthCustodyVault.BadQuorum.selector); vault.setPaused(true, 1, s);
        s = _sign(vault.pauseDigest(true, 1), 3, keys); vault.setPaused(true, 1, s);
        vm.expectRevert(RebirthCustodyVault.Paused.selector); vault.deposit(1, ACCOUNT);
        vm.warp(block.timestamp + 30);
        vm.expectRevert(RebirthCustodyVault.Paused.selector); vault.finalizeWithdrawal(w.id);
        _pause(false, 2, keys);
        vm.expectRevert(RebirthCustodyVault.BadPauseNonce.selector); vault.setPaused(true, 1, s);
        w = _w(2, 1);
        vm.expectRevert(RebirthCustodyVault.BadSignature.selector); vault.requestWithdrawal(w, s);
        vault.finalizeWithdrawal(bytes32(uint256(1))); _invariant();
    }
    function test_dual_quorum_delayed_rotation_requires_pause_and_new_keys() public {
        uint64 when = uint64(block.timestamp + 60);
        bytes[] memory oldS = _sign(vault.rotationDigest(nextRoster, when, 1), 3, keys);
        bytes[] memory newS = _sign(vault.acceptanceDigest(nextRoster, when, 1), 3, nextKeys);
        vm.expectRevert(RebirthCustodyVault.NotPaused.selector);
        vault.scheduleRotation(nextRoster, when, 1, oldS, newS);
        _pause(true, 1, keys);
        newS = _sign(vault.acceptanceDigest(nextRoster, when, 1), 2, nextKeys);
        vm.expectRevert(RebirthCustodyVault.BadQuorum.selector);
        vault.scheduleRotation(nextRoster, when, 1, oldS, newS);
        _schedule(when, 1);
        vm.expectRevert(RebirthCustodyVault.RotationWindowOpen.selector); vault.activateRotation();
        vm.warp(when); vault.activateRotation();
        _assert(vault.rosterEpoch() == 1 && vault.ROSTER_HASH() == keccak256(abi.encode(nextRoster)) && vault.paused());
        for (uint256 i; i < 4; ++i) _assert(!vault.isOperator(roster[i]) && vault.isOperator(nextRoster[i]));
        bytes[] memory bad = _sign(vault.pauseDigest(false, 2), 3, keys);
        vm.expectRevert(RebirthCustodyVault.BadSignature.selector); vault.setPaused(false, 2, bad);
        _pause(false, 2, nextKeys);
    }
    function test_rotation_cancellation_consumes_nonce_and_cannot_replay() public {
        _pause(true, 1, keys); uint64 when = uint64(block.timestamp + 60); _schedule(when, 1);
        bytes[] memory cancel = _sign(vault.cancelRotationDigest(), 3, keys);
        vault.cancelRotation(cancel);
        (bytes32 digest,,,,) = vault.pendingRotation();
        _assert(digest == bytes32(0) && vault.rotationNonce() == 1 && vault.rosterEpoch() == 0);
        vm.expectRevert(RebirthCustodyVault.BadRotation.selector); vault.cancelRotation(cancel);
        bytes[] memory s = _sign(vault.rotationDigest(nextRoster, when, 1), 3, keys);
        bytes[] memory ns = _sign(vault.acceptanceDigest(nextRoster, when, 1), 3, nextKeys);
        vm.expectRevert(RebirthCustodyVault.BadRotation.selector); vault.scheduleRotation(nextRoster, when, 1, s, ns);
        _schedule(when, 2);
    }
    function test_old_unaccepted_certificate_rejected_but_queued_claim_survives_rotation() public {
        vault.deposit(1_000_000, ACCOUNT);
        RebirthCustodyVault.Withdrawal memory accepted = _w(1, 250_000); _request(accepted);
        RebirthCustodyVault.Withdrawal memory unaccepted = _w(2, 250_000);
        bytes32 oldDigest = vault.withdrawalDigest(unaccepted);
        bytes[] memory stale = _sign(oldDigest, 3, keys);
        _pause(true, 1, keys); uint64 when = uint64(block.timestamp + 60); _schedule(when, 1);
        vm.warp(when); vault.activateRotation(); _pause(false, 2, nextKeys);
        _assert(vault.withdrawalDigest(unaccepted) != oldDigest);
        vm.expectRevert(RebirthCustodyVault.BadSignature.selector); vault.requestWithdrawal(unaccepted, stale);
        vault.finalizeWithdrawal(accepted.id);
        vault.requestWithdrawal(unaccepted, _sign(vault.withdrawalDigest(unaccepted), 3, nextKeys));
        _assert(vault.pendingTotal() == 250_000 && token.balanceOf(RECIPIENT) == 250_000); _invariant();
    }
    function test_bad_rotation_roster_short_delay_and_signature_substitution_refuse() public {
        _pause(true, 1, keys); uint64 when = uint64(block.timestamp + 60);
        address[4] memory duplicate = nextRoster; duplicate[1] = duplicate[0];
        bytes[] memory empty = new bytes[](0);
        vm.expectRevert(RebirthCustodyVault.BadConfiguration.selector);
        vault.scheduleRotation(duplicate, when, 1, empty, empty);
        vm.expectRevert(RebirthCustodyVault.BadRotation.selector);
        vault.scheduleRotation(nextRoster, when - 1, 1, empty, empty);
        bytes[] memory oldS = _sign(vault.rotationDigest(nextRoster, when, 1), 3, keys);
        bytes[] memory oldAsNew = _sign(vault.acceptanceDigest(nextRoster, when, 1), 3, keys);
        vm.expectRevert(RebirthCustodyVault.BadSignature.selector);
        vault.scheduleRotation(nextRoster, when, 1, oldS, oldAsNew);
    }
    function test_production_chain_cannot_use_short_regtest_delays() public {
        vm.chainId(42161);
        vm.expectRevert(RebirthCustodyVault.BadConfiguration.selector);
        _deploy(ASSET, INSTANCE, roster, 30, 60, 3600);
        // Pure local VM, no RPC/fork/deployment to Arbitrum. Generic contract
        // accepts only an explicit matching policy with conservative delays.
        RebirthCustodyVault generic = _deploy(ASSET, INSTANCE, roster, 300, 7 days, 3600);
        _assert(generic.DEPLOYMENT_CHAIN_ID() == 42161 && generic.DISPUTE_SECONDS() == 300);
        vm.expectRevert(RebirthCustodyVault.WrongChain.selector);
        new RebirthCustodyVault(address(token), ASSET, INSTANCE, roster,
            RebirthCustodyVault.Policy(31337, 30, 60, 3600));
    }
    function test_invalid_configuration_zero_asset_duplicate_and_bad_lifetime_refuse() public {
        address[4] memory duplicate = roster; duplicate[1] = duplicate[0];
        vm.expectRevert(RebirthCustodyVault.BadConfiguration.selector); _deploy(ASSET, INSTANCE, duplicate, 30, 60, 3600);
        vm.expectRevert(RebirthCustodyVault.BadConfiguration.selector); _deploy(bytes32(0), INSTANCE, roster, 30, 60, 3600);
        vm.expectRevert(RebirthCustodyVault.BadConfiguration.selector); _deploy(ASSET, bytes32(0), roster, 30, 60, 3600);
        vm.expectRevert(RebirthCustodyVault.BadConfiguration.selector); _deploy(ASSET, INSTANCE, roster, 30, 60, 0);
        vm.expectRevert(RebirthCustodyVault.BadConfiguration.selector); _deploy(ASSET, INSTANCE, roster, 30, 60, 86401);
    }
    function test_high_s_invalid_v_wrong_key_and_wrong_signature_length_refuse() public {
        vault.deposit(1_000_000, ACCOUNT); RebirthCustodyVault.Withdrawal memory w = _w(1, 1);
        bytes32 digest = vault.withdrawalDigest(w);
        bytes[] memory s = _sign(digest, 3, keys);
        (uint8 v, bytes32 r,) = vm.sign(keys[0], digest);
        s[0] = abi.encodePacked(r, bytes32(type(uint256).max), v);
        vm.expectRevert(RebirthCustodyVault.BadSignature.selector); vault.requestWithdrawal(w, s);
        s = _sign(digest, 3, keys); s[0][64] = bytes1(uint8(0));
        vm.expectRevert(RebirthCustodyVault.BadSignature.selector); vault.requestWithdrawal(w, s);
        s = _sign(digest, 3, nextKeys);
        vm.expectRevert(RebirthCustodyVault.BadSignature.selector); vault.requestWithdrawal(w, s);
        s = _sign(digest, 3, keys); s[0] = hex"01";
        vm.expectRevert(RebirthCustodyVault.BadSignature.selector); vault.requestWithdrawal(w, s);
    }
    function testFuzz_reserved_principal_and_actual_reserve_conserved(uint64 rawDeposit, uint64 rawFirst, uint64 rawSecond) public {
        uint256 amount = uint256(rawDeposit) % 9_000_000 + 1;
        uint256 first = uint256(rawFirst) % amount + 1;
        uint256 second = uint256(rawSecond) % (amount - first + 1);
        vault.deposit(amount, ACCOUNT); _request(_w(1, first));
        if (second != 0) _request(_w(2, second));
        _assert(vault.pendingTotal() == first + second); _invariant();
        vm.warp(block.timestamp + 30); vault.finalizeWithdrawal(bytes32(uint256(1)));
        _assert(vault.locked() == amount - first && vault.pendingTotal() == second); _invariant();
        if (second != 0) vault.finalizeWithdrawal(bytes32(uint256(2)));
        _assert(vault.locked() == amount - first - second && vault.pendingTotal() == 0);
        _assert(token.balanceOf(RECIPIENT) == first + second); _invariant();
    }
}
