// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {RebirthCustodyVault} from "custody/RebirthCustodyVault.sol";

interface VmArbitrumRehearsal {
    function warp(uint256 timestamp) external;
    function addr(uint256 key) external returns (address);
    function sign(uint256 key, bytes32 digest) external returns (uint8 v, bytes32 r, bytes32 s);
    function prank(address sender) external;
    function expectRevert(bytes4 selector) external;
    function deal(address account, uint256 amount) external;
    function chainId(uint256 chain) external;
}
interface IRealStable {
    function name() external view returns (string memory);
    function DOMAIN_SEPARATOR() external view returns (bytes32);
    function RECEIVE_WITH_AUTHORIZATION_TYPEHASH() external view returns (bytes32);
    function authorizationState(address authorizer, bytes32 nonce) external view returns (bool);
    function allowance(address owner, address spender) external view returns (uint256);
    function decimals() external view returns (uint8);
    function balanceOf(address account) external view returns (uint256);
    function approve(address spender, uint256 amount) external returns (bool);
    function transfer(address recipient, uint256 amount) external returns (bool);
}
interface IRealUSDT0 is IRealStable {
    function owner() external view returns (address);
    function oftContract() external view returns (address);
    function mint(address recipient, uint256 amount) external;
    function addToBlockedList(address account) external;
    function removeFromBlockedList(address account) external;
    function destroyBlockedFunds(address account) external;
}
interface IRealUSDC is IRealStable {
    function masterMinter() external view returns (address);
    function configureMinter(address minter, uint256 allowance) external returns (bool);
    function mint(address recipient, uint256 amount) external returns (bool);
    function blacklister() external view returns (address);
    function blacklist(address account) external;
    function unBlacklist(address account) external;
    function pauser() external view returns (address);
    function pause() external;
    function unpause() external;
}

/// OPT-IN REHEARSAL ONLY. Run against an owned chain31337 fork of an explicitly
/// recorded finalized Arbitrum block. Opt in with FOUNDRY_TEST pointing to this
/// directory, outside the default contracts source tree and ordinary tests.
/// No RPC connection, real key, etch, mocked token or storage writes occur here.
/// Funding and issuer faults impersonate real privileged roles LOCALLY only.
/// This exercises real token code; it does not qualify B3 consensus or mainnet.
contract RebirthArbitrumForkTest {
    VmArbitrumRehearsal private constant vm = VmArbitrumRehearsal(address(uint160(uint256(keccak256("hevm cheat code")))));
    IRealUSDT0 private constant USDT0 = IRealUSDT0(0xFd086bC7CD5C481DCC9C85ebE478A1C0b69FCbb9);
    IRealUSDC private constant USDC = IRealUSDC(0xaf88d065e77c8cC2239327C5EDb3A432268e5831);
    bytes32 private constant ASSET = keccak256("LOCAL ONLY real-token custody rehearsal stable");
    bytes32 private constant ACCOUNT = keccak256("LOCAL ONLY rehearsal account");
    uint256[4] private keys;
    address[4] private roster;
    address private recipient;
    uint256 private constant SOURCE_KEY = 987655;
    struct Sponsored {
        address source;
        uint256 amount;
        bytes32 account;
        bytes32 salt;
        uint256 validAfter;
        uint256 validBefore;
        uint8 v;
        bytes32 r;
        bytes32 s;
    }

    function setUp() public {
        require(block.chainid == 31337, "rehearsal refuses real chain42161 execution");
        require(address(USDT0).code.length != 0 && address(USDC).code.length != 0, "requires real Arbitrum token fork");
        require(USDT0.decimals() == 6 && USDC.decimals() == 6, "token decimals changed");
        recipient = vm.addr(987654);
        for (uint256 i; i < 4; ++i) { keys[i] = 1101 + i; roster[i] = vm.addr(keys[i]); }
        for (uint256 i; i < 4; ++i) for (uint256 j = i + 1; j < 4; ++j)
            if (roster[j] < roster[i]) { (roster[i], roster[j]) = (roster[j], roster[i]); (keys[i], keys[j]) = (keys[j], keys[i]); }
    }

    function _fund(IRealStable token) private {
        uint256 beforeBalance = token.balanceOf(address(this));
        if (address(token) == address(USDT0)) {
            address gateway = USDT0.oftContract();
            require(gateway != address(0), "USDT0 issuer gateway missing");
            vm.prank(gateway);
            USDT0.mint(address(this), 10_000_000);
        } else {
            address master = USDC.masterMinter();
            require(master != address(0), "USDC master minter missing");
            vm.prank(master);
            require(USDC.configureMinter(address(this), 10_000_000), "local USDC minter fixture refused");
            require(USDC.mint(address(this), 10_000_000), "local USDC funding fixture refused");
        }
        require(token.balanceOf(address(this)) - beforeBalance == 10_000_000, "non-exact local funding");
    }
    function _deposit(IRealStable token) private returns (RebirthCustodyVault vault) {
        _fund(token);
        vault = new RebirthCustodyVault(address(token), ASSET, keccak256(abi.encode("LOCAL fork", address(token))), roster,
            RebirthCustodyVault.Policy(31337, 300, 7 days, 600));
        // Zero-first approval also supports legacy USDT allowance behavior.
        require(token.approve(address(vault), 0) && token.approve(address(vault), 1_000_000), "approve failed");
        uint256 beforeSender = token.balanceOf(address(this));
        uint256 beforeVault = token.balanceOf(address(vault));
        vault.deposit(1_000_000, ACCOUNT);
        require(beforeSender - token.balanceOf(address(this)) == 1_000_000, "unexpected deposit sender charge");
        // Fork addresses may already own tokens. Existing surplus creates no
        // credit; only the exact incoming principal becomes a liability.
        require(token.balanceOf(address(vault)) - beforeVault == 1_000_000 && vault.locked() == 1_000_000, "deposit mismatch");
        require(vault.nextDepositId() == 1, "deposit cursor mismatch");
    }
    function _request(RebirthCustodyVault vault) private returns (bytes32 id) {
        id = keccak256("LOCAL ONLY committed withdrawal fixture");
        RebirthCustodyVault.Withdrawal memory w = RebirthCustodyVault.Withdrawal(id, ACCOUNT, ASSET, 250_000,
            recipient, 1, keccak256("LOCAL ONLY state-root fixture"), uint64(block.timestamp + 240));
        bytes32 digest = vault.withdrawalDigest(w);
        bytes[] memory signatures = new bytes[](3);
        for (uint256 i; i < 3; ++i) {
            (uint8 v, bytes32 r, bytes32 s) = vm.sign(keys[i], digest);
            signatures[i] = abi.encodePacked(r, s, v);
        }
        vault.requestWithdrawal(w, signatures);
        require(vault.pendingTotal() == 250_000 && vault.locked() == 1_000_000, "reservation mismatch");
    }
    function _blocked(IRealStable token, address account, bool blocked) private {
        if (address(token) == address(USDT0)) {
            address issuer = USDT0.owner();
            vm.prank(issuer);
            if (blocked) USDT0.addToBlockedList(account); else USDT0.removeFromBlockedList(account);
        } else {
            address issuer = USDC.blacklister();
            vm.prank(issuer);
            if (blocked) USDC.blacklist(account); else USDC.unBlacklist(account);
        }
    }
    function _preserved(RebirthCustodyVault vault, bytes32 id) private view {
        (,,,, bool paid) = vault.withdrawals(id);
        require(!paid && vault.locked() == 1_000_000 && vault.pendingTotal() == 250_000, "failed payout consumed claim");
    }
    function _roundtrip(IRealStable token) private {
        RebirthCustodyVault vault = _deposit(token);
        uint256 initialSurplus = token.balanceOf(address(vault)) - vault.locked();
        require(token.transfer(address(vault), 7), "local donation failed");
        require(vault.locked() == 1_000_000 && vault.nextDepositId() == 1 &&
            token.balanceOf(address(vault)) == 1_000_007 + initialSurplus, "donation created bridge credit");
        bytes32 id = _request(vault);
        vm.expectRevert(RebirthCustodyVault.DisputeWindowOpen.selector);
        vault.finalizeWithdrawal(id);
        vm.warp(block.timestamp + 300);
        uint256 beforeRecipient = token.balanceOf(recipient);
        vault.finalizeWithdrawal(id);
        require(token.balanceOf(recipient) - beforeRecipient == 250_000, "non-exact real-token payout");
        require(vault.locked() == 750_000 && vault.pendingTotal() == 0 && token.balanceOf(address(vault)) == 750_007 + initialSurplus,
            "real-token reserves do not reconcile");
        vm.expectRevert(RebirthCustodyVault.AlreadyPaid.selector);
        vault.finalizeWithdrawal(id);
    }
    function _blocklist(IRealStable token) private {
        RebirthCustodyVault vault = _deposit(token);
        bytes32 id = _request(vault);
        vm.warp(block.timestamp + 300);
        _blocked(token, address(vault), true);
        vm.expectRevert(RebirthCustodyVault.TransferFailed.selector);
        vault.finalizeWithdrawal(id);
        _preserved(vault, id);
        _blocked(token, address(vault), false);
        uint256 beforeRecipient = token.balanceOf(recipient);
        vault.finalizeWithdrawal(id);
        require(token.balanceOf(recipient) - beforeRecipient == 250_000 && vault.pendingTotal() == 0, "unblocked claim retry failed");
    }
    function test_usdt0_exact_deposit_donation_claim_payout_and_replay() public { _roundtrip(USDT0); }
    function test_usdc_exact_deposit_donation_claim_payout_and_replay() public { _roundtrip(USDC); }
    function test_usdt0_issuer_blocklist_retains_claim_and_same_id_retries() public { _blocklist(USDT0); }
    function test_usdc_issuer_blocklist_retains_claim_and_same_id_retries() public { _blocklist(USDC); }
    function test_usdc_issuer_pause_retains_claim_and_same_id_retries() public {
        RebirthCustodyVault vault = _deposit(USDC);
        bytes32 id = _request(vault);
        vm.warp(block.timestamp + 300);
        address issuer = USDC.pauser();
        vm.prank(issuer); USDC.pause();
        vm.expectRevert(RebirthCustodyVault.TransferFailed.selector); vault.finalizeWithdrawal(id);
        _preserved(vault, id);
        vm.prank(issuer); USDC.unpause();
        vault.finalizeWithdrawal(id);
        require(vault.pendingTotal() == 0 && vault.locked() == 750_000, "unpaused claim retry failed");
    }
    function test_usdt0_issuer_destroyed_reserves_fail_closed_preserving_obligation() public {
        RebirthCustodyVault vault = _deposit(USDT0);
        bytes32 id = _request(vault);
        _blocked(USDT0, address(vault), true);
        address issuer = USDT0.owner();
        vm.prank(issuer); USDT0.destroyBlockedFunds(address(vault));
        _blocked(USDT0, address(vault), false);
        require(USDT0.balanceOf(address(vault)) == 0, "issuer destruction fixture did not remove reserve");
        vm.warp(block.timestamp + 300);
        vm.expectRevert(RebirthCustodyVault.InsufficientReserve.selector); vault.finalizeWithdrawal(id);
        _preserved(vault, id);
    }

    function _sponsoredSetup(IRealStable token) private returns (RebirthCustodyVault vault, address source) {
        _fund(token);
        source = vm.addr(SOURCE_KEY);
        vm.deal(source, 0);  // Artificial local ETH fixture only; token balances are never overwritten.
        require(token.transfer(source, 2_000_000), "source fixture transfer failed");
        vault = new RebirthCustodyVault(address(token), ASSET, keccak256(abi.encode("LOCAL sponsored", address(token))), roster,
            RebirthCustodyVault.Policy(31337, 300, 7 days, 600));
        require(source.balance == 0 && token.allowance(source, address(vault)) == 0, "source should need no ETH/approval");
    }
    function _sponsored(IRealStable token, RebirthCustodyVault vault, uint256 after_, uint256 before_, bytes32 domain)
        private returns (Sponsored memory a)
    {
        a.source = vm.addr(SOURCE_KEY); a.amount = 1_000_000; a.account = ACCOUNT;
        a.salt = keccak256("LOCAL ONLY unique authorization salt"); a.validAfter = after_; a.validBefore = before_;
        bytes32 nonce = vault.depositAuthorizationNonce(a.source, a.amount, a.account, a.salt);
        bytes32 hash_ = keccak256(abi.encode(token.RECEIVE_WITH_AUTHORIZATION_TYPEHASH(), a.source, address(vault),
            a.amount, a.validAfter, a.validBefore, nonce));
        (a.v, a.r, a.s) = vm.sign(SOURCE_KEY, keccak256(abi.encodePacked(hex"1901", domain, hash_)));
    }
    function _submit(RebirthCustodyVault vault, Sponsored memory a) private {
        vault.depositWithAuthorization(a.source, a.amount, a.account, a.salt, a.validAfter, a.validBefore, a.v, a.r, a.s);
    }
    function _noSponsoredCredit(IRealStable token, RebirthCustodyVault vault, Sponsored memory original) private view {
        require(vault.locked() == 0 && vault.nextDepositId() == 0 && token.balanceOf(address(vault)) == 0,
            "refused authorization created credit");
        require(!token.authorizationState(original.source,
            vault.depositAuthorizationNonce(original.source, original.amount, original.account, original.salt)),
            "refused authorization consumed source nonce");
    }
    function _sponsoredPositive(IRealStable token) private {
        (RebirthCustodyVault vault, address source) = _sponsoredSetup(token);
        Sponsored memory a = _sponsored(token, vault, block.timestamp - 1, block.timestamp + 240, token.DOMAIN_SEPARATOR());
        uint256 beforeSender = token.balanceOf(source);
        _submit(vault, a);  // The test sponsor is caller; the zero-ETH source sends nothing.
        require(source.balance == 0 && beforeSender - token.balanceOf(source) == 1_000_000, "source charged extra assets");
        require(vault.locked() == 1_000_000 && vault.nextDepositId() == 1 && token.balanceOf(address(vault)) == 1_000_000,
            "sponsored principal mismatch");
        require(token.allowance(source, address(vault)) == 0, "sponsored deposit unexpectedly used approve");
        vm.expectRevert(RebirthCustodyVault.TransferFailed.selector); _submit(vault, a);
        require(vault.locked() == 1_000_000 && vault.nextDepositId() == 1, "sponsored replay changed credit");
        bytes32 id = _request(vault);
        vm.warp(block.timestamp + 300); vault.finalizeWithdrawal(id);
        require(vault.locked() == 750_000 && vault.pendingTotal() == 0, "sponsored withdrawal reserves mismatch");
    }
    function _sponsoredSubstitution(IRealStable token) private {
        (RebirthCustodyVault vault,) = _sponsoredSetup(token);
        Sponsored memory original = _sponsored(token, vault, block.timestamp - 1, block.timestamp + 240, token.DOMAIN_SEPARATOR());
        for (uint256 field; field < 6; ++field) {
            Sponsored memory bad = abi.decode(abi.encode(original), (Sponsored));
            if (field == 0) bad.account = keccak256("redirect B3 account");
            else if (field == 1) ++bad.amount;
            else if (field == 2) bad.salt = keccak256("substituted salt");
            else if (field == 3) bad.source = recipient;
            else if (field == 4) --bad.validAfter;
            else ++bad.validBefore;
            vm.expectRevert(RebirthCustodyVault.TransferFailed.selector); _submit(vault, bad);
            _noSponsoredCredit(token, vault, original);
        }
        RebirthCustodyVault other = new RebirthCustodyVault(address(token), ASSET, vault.INSTANCE_ID(), roster,
            RebirthCustodyVault.Policy(31337, 300, 7 days, 600));
        vm.expectRevert(RebirthCustodyVault.TransferFailed.selector); _submit(other, original);
        _noSponsoredCredit(token, vault, original);
        _noSponsoredCredit(token, other, original);
        _submit(vault, original);  // Refused substitutions did not consume the genuine token nonce.
    }
    function _sponsoredTimeAndChain(IRealStable token) private {
        (RebirthCustodyVault vault,) = _sponsoredSetup(token);
        Sponsored memory future = _sponsored(token, vault, block.timestamp + 1, block.timestamp + 240, token.DOMAIN_SEPARATOR());
        vm.expectRevert(RebirthCustodyVault.TransferFailed.selector); _submit(vault, future);
        _noSponsoredCredit(token, vault, future);
        Sponsored memory expired = _sponsored(token, vault, block.timestamp - 20, block.timestamp - 1, token.DOMAIN_SEPARATOR());
        vm.expectRevert(RebirthCustodyVault.TransferFailed.selector); _submit(vault, expired);
        bytes32 wrongDomain = keccak256(abi.encode(
            keccak256("EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)"),
            keccak256(bytes(token.name())), keccak256(bytes(address(token) == address(USDT0) ? "1" : "2")),
            uint256(42161), address(token)));
        Sponsored memory wrongChain = _sponsored(token, vault, block.timestamp - 1, block.timestamp + 240, wrongDomain);
        vm.expectRevert(RebirthCustodyVault.TransferFailed.selector); _submit(vault, wrongChain);
        _noSponsoredCredit(token, vault, wrongChain);
        vm.chainId(42161);
        vm.expectRevert(RebirthCustodyVault.WrongChain.selector); _submit(vault, wrongChain);
        vm.chainId(31337);
    }
    function _sponsoredBlocked(IRealStable token) private {
        (RebirthCustodyVault vault, address source) = _sponsoredSetup(token);
        Sponsored memory a = _sponsored(token, vault, block.timestamp - 1, block.timestamp + 240, token.DOMAIN_SEPARATOR());
        _blocked(token, source, true);
        vm.expectRevert(RebirthCustodyVault.TransferFailed.selector); _submit(vault, a);
        _noSponsoredCredit(token, vault, a);
        _blocked(token, source, false); _blocked(token, address(vault), true);
        vm.expectRevert(RebirthCustodyVault.TransferFailed.selector); _submit(vault, a);
        _noSponsoredCredit(token, vault, a);
        _blocked(token, address(vault), false); _submit(vault, a);
        require(vault.locked() == 1_000_000 && source.balance == 0, "unblocked sponsored retry failed");
    }
    function test_usdt0_sponsored_zero_eth_source_no_approve_and_token_nonce_replay_refused() public { _sponsoredPositive(USDT0); }
    function test_usdc_sponsored_zero_eth_source_no_approve_and_token_nonce_replay_refused() public { _sponsoredPositive(USDC); }
    function test_usdt0_sponsored_account_amount_salt_source_time_vault_substitution_refuses() public { _sponsoredSubstitution(USDT0); }
    function test_usdc_sponsored_account_amount_salt_source_time_vault_substitution_refuses() public { _sponsoredSubstitution(USDC); }
    function test_usdt0_sponsored_expired_future_wrong_chain_refuses() public { _sponsoredTimeAndChain(USDT0); }
    function test_usdc_sponsored_expired_future_wrong_chain_refuses() public { _sponsoredTimeAndChain(USDC); }
    function test_usdt0_sponsored_issuer_blocklist_refuses_without_nonce_or_credit_loss() public { _sponsoredBlocked(USDT0); }
    function test_usdc_sponsored_issuer_blocklist_refuses_without_nonce_or_credit_loss() public { _sponsoredBlocked(USDC); }
}
