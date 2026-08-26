// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {Test} from "forge-std/Test.sol";
import {B3DepositVault} from "../B3DepositVault.sol";

/// Standard-compliant ERC-20 (returns bool).
contract MockStandardToken {
    mapping(address => uint256) public balanceOf;
    mapping(address => mapping(address => uint256)) public allowance;

    function mint(address to, uint256 amount) external { balanceOf[to] += amount; }
    function approve(address spender, uint256 amount) external returns (bool) {
        allowance[msg.sender][spender] = amount;
        return true;
    }
    function transfer(address to, uint256 amount) external returns (bool) {
        balanceOf[msg.sender] -= amount;
        balanceOf[to] += amount;
        return true;
    }
    function transferFrom(address from, address to, uint256 amount) external returns (bool) {
        allowance[from][msg.sender] -= amount;
        balanceOf[from] -= amount;
        balanceOf[to] += amount;
        return true;
    }
}

/// USDT-style token: transfer/transferFrom return NOTHING (no bool).
contract MockUSDT {
    mapping(address => uint256) public balanceOf;
    mapping(address => mapping(address => uint256)) public allowance;

    function mint(address to, uint256 amount) external { balanceOf[to] += amount; }
    function approve(address spender, uint256 amount) external {
        allowance[msg.sender][spender] = amount;
    }
    function transfer(address to, uint256 amount) external {
        balanceOf[msg.sender] -= amount;
        balanceOf[to] += amount;
    }
    function transferFrom(address from, address to, uint256 amount) external {
        allowance[from][msg.sender] -= amount;
        balanceOf[from] -= amount;
        balanceOf[to] += amount;
    }
}

/// Fee-on-transfer token: recipient receives 99%.
contract MockFeeToken {
    mapping(address => uint256) public balanceOf;
    mapping(address => mapping(address => uint256)) public allowance;

    function mint(address to, uint256 amount) external { balanceOf[to] += amount; }
    function approve(address spender, uint256 amount) external returns (bool) {
        allowance[msg.sender][spender] = amount;
        return true;
    }
    function transferFrom(address from, address to, uint256 amount) external returns (bool) {
        allowance[from][msg.sender] -= amount;
        balanceOf[from] -= amount;
        uint256 fee = amount / 100;
        balanceOf[to] += amount - fee;
        return true;
    }
    function transfer(address to, uint256 amount) external returns (bool) {
        balanceOf[msg.sender] -= amount;
        balanceOf[to] += amount - amount / 100;
        return true;
    }
}

contract RejectingReceiver {
    receive() external payable { revert("no"); }
}

/// Hook-style malicious token: transferFrom reenters depositToken to try
/// the balance-delta double count.
contract ReentrantToken {
    B3DepositVault public vault;
    uint256 public balance_;
    bool private attacking;

    function setVault(B3DepositVault v) external { vault = v; }
    function balanceOf(address) external view returns (uint256) { return balance_; }
    function approve(address, uint256) external returns (bool) { return true; }
    function transfer(address, uint256) external returns (bool) { return true; }

    function transferFrom(address, address, uint256 amount) external returns (bool) {
        balance_ += amount;
        if (!attacking) {
            attacking = true;
            vault.depositToken(address(this), amount / 2, bytes32(0)); // reenter
            attacking = false;
        }
        return true;
    }
}

contract B3DepositVaultTest is Test {
    event Deposit(uint64 indexed depositId, address indexed token, uint256 amount, bytes32 b3Recipient);
    event Released(address indexed token, address indexed to, uint256 amount);
    event Rescued(address indexed token, address indexed to, uint256 amount);

    address authority = address(0xA11CE);
    address rescuer = address(0x5AFE);
    B3DepositVault vault;
    MockStandardToken std_;
    MockUSDT usdt;
    MockFeeToken fee;

    function setUp() public {
        vault = new B3DepositVault(authority, rescuer);
        std_ = new MockStandardToken();
        usdt = new MockUSDT();
        fee = new MockFeeToken();
        std_.mint(address(this), 1e24);
        usdt.mint(address(this), 1e12);
        fee.mint(address(this), 1e24);
    }

    function test_ZeroAuthorityReverts() public {
        vm.expectRevert(B3DepositVault.ZeroAuthority.selector);
        new B3DepositVault(address(0), rescuer);
        vm.expectRevert(B3DepositVault.ZeroAuthority.selector);
        new B3DepositVault(authority, address(0));
    }

    function test_DepositETH_SequentialIdsAndEvent() public {
        vm.expectEmit(true, true, false, true, address(vault));
        emit Deposit(0, address(0), 1 ether, bytes32(uint256(0x99)));
        vault.depositETH{value: 1 ether}(bytes32(uint256(0x99)));

        vm.expectEmit(true, true, false, true, address(vault));
        emit Deposit(1, address(0), 2 ether, bytes32(uint256(0x77)));
        vault.depositETH{value: 2 ether}(bytes32(uint256(0x77)));

        assertEq(vault.nextDepositId(), 2);
        assertEq(address(vault).balance, 3 ether);
    }

    function test_DepositETH_ZeroReverts() public {
        vm.expectRevert(B3DepositVault.ZeroAmount.selector);
        vault.depositETH{value: 0}(bytes32(0));
    }

    function test_DepositToken_Standard() public {
        std_.approve(address(vault), 500);
        vm.expectEmit(true, true, false, true, address(vault));
        emit Deposit(0, address(std_), 500, bytes32(uint256(1)));
        vault.depositToken(address(std_), 500, bytes32(uint256(1)));
        assertEq(std_.balanceOf(address(vault)), 500);
    }

    function test_DepositToken_USDTStyleNoReturnValue() public {
        usdt.approve(address(vault), 1_000_000); // 1 USDT (6 decimals)
        vm.expectEmit(true, true, false, true, address(vault));
        emit Deposit(0, address(usdt), 1_000_000, bytes32(uint256(2)));
        vault.depositToken(address(usdt), 1_000_000, bytes32(uint256(2)));
        assertEq(usdt.balanceOf(address(vault)), 1_000_000);
    }

    function test_DepositToken_FeeOnTransfer_EmitsReceivedAmount() public {
        fee.approve(address(vault), 1000);
        vm.expectEmit(true, true, false, true, address(vault));
        emit Deposit(0, address(fee), 990, bytes32(uint256(3))); // balance delta, not 1000
        vault.depositToken(address(fee), 1000, bytes32(uint256(3)));
    }

    function test_DepositToken_ZeroReverts() public {
        vm.expectRevert(B3DepositVault.ZeroAmount.selector);
        vault.depositToken(address(std_), 0, bytes32(0));
    }

    function test_Release_OnlyAuthority() public {
        vault.depositETH{value: 1 ether}(bytes32(0));
        vm.expectRevert(B3DepositVault.NotAuthority.selector);
        vault.release(address(0), payable(address(this)), 1 ether);
    }

    function test_Release_ETH() public {
        vault.depositETH{value: 1 ether}(bytes32(0));
        address payable to = payable(address(0xBEEF));
        vm.prank(authority);
        vm.expectEmit(true, true, false, true, address(vault));
        emit Released(address(0), to, 0.4 ether);
        vault.release(address(0), to, 0.4 ether);
        assertEq(to.balance, 0.4 ether);
        assertEq(address(vault).balance, 0.6 ether);
    }

    function test_Release_Token_USDTStyle() public {
        usdt.approve(address(vault), 1_000_000);
        vault.depositToken(address(usdt), 1_000_000, bytes32(0));
        vm.prank(authority);
        vault.release(address(usdt), payable(address(0xBEEF)), 250_000);
        assertEq(usdt.balanceOf(address(0xBEEF)), 250_000);
        assertEq(usdt.balanceOf(address(vault)), 750_000);
    }

    function test_DepositToken_ReentrancyBlocked() public {
        ReentrantToken evil = new ReentrantToken();
        evil.setVault(vault);
        // The inner reentrant call reverts with Reentrancy(); the outer
        // safe-call wrapper surfaces that as TransferFailed. Either way the
        // double-counted deposit can never happen.
        vm.expectRevert(B3DepositVault.TransferFailed.selector);
        vault.depositToken(address(evil), 1000, bytes32(0));
        assertEq(vault.nextDepositId(), 0); // no event escaped
    }

    function test_Rescue_OnlySurplus_ETH() public {
        vault.depositETH{value: 1 ether}(bytes32(0)); // locked = 1 ether
        vm.deal(address(vault), 1.5 ether);           // 0.5 ether force-sent stray
        address payable out = payable(address(0xCAFE));

        vm.prank(rescuer);
        vm.expectEmit(true, true, false, true, address(vault));
        emit Rescued(address(0), out, 0.5 ether);
        vault.rescue(address(0), out);

        assertEq(out.balance, 0.5 ether);
        assertEq(address(vault).balance, 1 ether);    // locked funds untouched

        vm.prank(rescuer);
        vm.expectRevert(B3DepositVault.NothingToRescue.selector);
        vault.rescue(address(0), out);                 // nothing left above liabilities
    }

    function test_Rescue_OnlySurplus_Token() public {
        std_.approve(address(vault), 500);
        vault.depositToken(address(std_), 500, bytes32(0)); // locked = 500
        std_.mint(address(vault), 200);                     // stray airdrop
        address payable out = payable(address(0xCAFE));

        vm.prank(rescuer);
        vault.rescue(address(std_), out);
        assertEq(std_.balanceOf(out), 200);
        assertEq(std_.balanceOf(address(vault)), 500);
    }

    function test_Rescue_NotRescuerReverts() public {
        vm.deal(address(vault), 1 ether);
        vm.expectRevert(B3DepositVault.NotAuthority.selector);
        vault.rescue(address(0), payable(address(this)));
    }

    function test_Release_ReducesLocked_ThenRescueStillBounded() public {
        vault.depositETH{value: 1 ether}(bytes32(0));
        vm.prank(authority);
        vault.release(address(0), payable(address(0xBEEF)), 0.4 ether);
        assertEq(vault.locked(address(0)), 0.6 ether);
        // Remaining 0.6 is liability; no surplus exists.
        vm.prank(rescuer);
        vm.expectRevert(B3DepositVault.NothingToRescue.selector);
        vault.rescue(address(0), payable(address(0xCAFE)));
    }

    function test_Release_ETH_ToRejectingReceiverReverts() public {
        vault.depositETH{value: 1 ether}(bytes32(0));
        RejectingReceiver rr = new RejectingReceiver();
        vm.prank(authority);
        vm.expectRevert(B3DepositVault.TransferFailed.selector);
        vault.release(address(0), payable(address(rr)), 1 ether);
    }
}
