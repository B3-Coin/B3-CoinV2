// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

// TEST ONLY doubles for test/v2/VaultV2.t.sol. The main token of the vault
// tests is contracts/regtest/MockTetherToken.sol; these cover the other token
// behaviours, F_d factory answers and caller shapes the vault must handle.

/// F_d factory stand-in. `isAccount` answers the raw word set per account
/// (B3VaultV2 accepts exactly 32 bytes equal to 1), or a malformed answer.
contract MockForwarderFactoryV2 {
    enum Answer {
        WORD, // 32-byte word from answerOf
        LONG, // 64 bytes: answerOf word then zero
        EMPTY, // no return data
        REVERT
    }

    mapping(address account => uint256 word) public answerOf;
    Answer public mode;

    function setAccount(address account, uint256 word) external {
        answerOf[account] = word;
    }

    function setMode(Answer newMode) external {
        mode = newMode;
    }

    /// Same selector as the factory's `isAccount(address) returns (bool)`;
    /// returning uint256 lets the test send words other than 0 and 1.
    function isAccount(address account) external view returns (uint256) {
        uint256 word = answerOf[account];
        Answer m = mode;
        if (m == Answer.REVERT) revert("factory revert");
        if (m == Answer.EMPTY) {
            assembly ("memory-safe") {
                return(0, 0)
            }
        }
        if (m == Answer.LONG) {
            assembly ("memory-safe") {
                mstore(0x00, word)
                mstore(0x20, 0)
                return(0x00, 0x40)
            }
        }
        return word;
    }
}

/// Minimal token interface used by the caller doubles.
interface ITokenV2Test {
    function approve(address spender, uint256 amount) external;
}

interface IVaultDepositV2Test {
    function deposit(uint256 amount, bytes32 b3Recipient) external;
}

/// Stands for an F_d account (B3Forwarder): a contract that calls
/// vault.deposit itself, holding the tokens and the allowance.
contract ForwarderAccountV2 {
    function depositVia(address vault, address token, uint256 amount, bytes32 b3Recipient) external {
        ITokenV2Test(token).approve(vault, 0);
        ITokenV2Test(token).approve(vault, amount);
        IVaultDepositV2Test(vault).deposit(amount, b3Recipient);
    }
}

/// A Safe-like smart wallet: executes arbitrary calls for its owner and
/// bubbles any revert.
contract SafeLikeCallerV2 {
    function exec(address target, bytes calldata data) external returns (bytes memory result) {
        bool ok;
        (ok, result) = target.call(data);
        if (!ok) {
            assembly ("memory-safe") {
                revert(add(result, 0x20), mload(result))
            }
        }
    }
}

/// Shared ERC-20 bookkeeping for the token doubles below (returns bool).
abstract contract TestTokenBaseV2 {
    mapping(address account => uint256) public balanceOf;
    mapping(address holder => mapping(address spender => uint256)) public allowance;

    event Transfer(address indexed from, address indexed to, uint256 value);

    function mint(address to, uint256 amount) external {
        balanceOf[to] += amount;
        emit Transfer(address(0), to, amount);
    }

    function approve(address spender, uint256 amount) external returns (bool) {
        allowance[msg.sender][spender] = amount;
        return true;
    }

    function _spend(address from, uint256 amount) internal {
        uint256 current = allowance[from][msg.sender];
        if (current != type(uint256).max) allowance[from][msg.sender] = current - amount;
    }

    function _move(address from, address to, uint256 amount) internal {
        balanceOf[from] -= amount;
        balanceOf[to] += amount;
        emit Transfer(from, to, amount);
    }
}

/// Tether status views, all false until set.
abstract contract TetherFlagsV2 {
    bool public paused;
    bool public deprecated;
    mapping(address account => bool) public isBlackListed;

    function setPaused(bool value) external {
        paused = value;
    }

    function setDeprecated(bool value) external {
        deprecated = value;
    }

    function setBlackListed(address account, bool value) external {
        isBlackListed[account] = value;
    }
}

/// A token whose transfers can return false, silently move nothing, or charge
/// a fixed fee (USDT's optional fee), with Tether's status views.
contract FalseReturnTokenV2 is TestTokenBaseV2, TetherFlagsV2 {
    enum Mode {
        NORMAL, // move, return true
        RETURN_FALSE, // move nothing, return false
        NO_OP, // move nothing, return true
        FEE // move amount - fee, burn the fee, return true
    }

    Mode public mode;
    uint256 public fee;

    function setMode(Mode newMode) external {
        mode = newMode;
    }

    function setFee(uint256 newFee) external {
        fee = newFee;
    }

    function transfer(address to, uint256 amount) external returns (bool) {
        return _transferFrom(msg.sender, to, amount);
    }

    function transferFrom(address from, address to, uint256 amount) external returns (bool) {
        Mode m = mode;
        if (m == Mode.NORMAL || m == Mode.FEE) _spend(from, amount);
        return _transferFrom(from, to, amount);
    }

    function _transferFrom(address from, address to, uint256 amount) private returns (bool) {
        Mode m = mode;
        if (m == Mode.RETURN_FALSE) return false;
        if (m == Mode.NO_OP) return true;
        if (m == Mode.FEE) {
            _move(from, to, amount - fee);
            balanceOf[from] -= fee;
            emit Transfer(from, address(0), fee);
            return true;
        }
        _move(from, to, amount);
        return true;
    }
}

/// A token that calls back into a target (the vault) from inside transfer
/// and transferFrom, records the inner call's outcome and then completes the
/// transfer. The hook fires once per arming.
contract ReentrantTokenV2 is TestTokenBaseV2, TetherFlagsV2 {
    address public hookTarget;
    bytes public hookData;
    bool public hookFired;
    bool public lastHookOk;
    bytes public lastHookReturn;

    function arm(address target, bytes calldata data) external {
        hookTarget = target;
        hookData = data;
        hookFired = false;
    }

    function transfer(address to, uint256 amount) external returns (bool) {
        _hook();
        _move(msg.sender, to, amount);
        return true;
    }

    function transferFrom(address from, address to, uint256 amount) external returns (bool) {
        _hook();
        _spend(from, amount);
        _move(from, to, amount);
        return true;
    }

    function _hook() private {
        address target = hookTarget;
        if (target == address(0) || hookFired) return;
        hookFired = true;
        (lastHookOk, lastHookReturn) = target.call(hookData);
    }
}

/// A plain ERC-20 without Tether's paused / deprecated / isBlackListed views.
contract NoFlagsTokenV2 is TestTokenBaseV2 {
    function transfer(address to, uint256 amount) external returns (bool) {
        _move(msg.sender, to, amount);
        return true;
    }

    function transferFrom(address from, address to, uint256 amount) external returns (bool) {
        _spend(from, amount);
        _move(from, to, amount);
        return true;
    }
}

/// Tether-like token whose paused() answers arbitrary raw bytes, to pin the
/// vault's "exactly 32 bytes, all zero" rule. deprecated and isBlackListed
/// answer false.
contract OddFlagsTokenV2 is TestTokenBaseV2 {
    bytes public pausedAnswer = abi.encode(false);
    bool public deprecated;
    mapping(address account => bool) public isBlackListed;

    function setPausedAnswer(bytes calldata answer) external {
        pausedAnswer = answer;
    }

    function paused() external view returns (bool) {
        bytes memory answer = pausedAnswer;
        assembly ("memory-safe") {
            return(add(answer, 0x20), mload(answer))
        }
    }

    function transfer(address to, uint256 amount) external returns (bool) {
        _move(msg.sender, to, amount);
        return true;
    }

    function transferFrom(address from, address to, uint256 amount) external returns (bool) {
        _spend(from, amount);
        _move(from, to, amount);
        return true;
    }
}

/// Answers the identity views a B-23 successor reads from its predecessor
/// (B3_ASSET_ID, ORIGIN_TOKEN, B3_CHAIN_DOMAIN, ORIGIN_CHAIN_ID) with
/// arbitrary values. Lets a test give a vault a chosen AssetId, such as the
/// 0x11..11 stand-in of the spec §6.4 leaf vector.
contract IdentityStubV2 {
    bytes32 public immutable B3_ASSET_ID;
    address public immutable ORIGIN_TOKEN;
    bytes32 public immutable B3_CHAIN_DOMAIN;
    uint64 public immutable ORIGIN_CHAIN_ID;

    constructor(bytes32 assetId, address token, bytes32 chainDomain, uint64 chainId) {
        B3_ASSET_ID = assetId;
        ORIGIN_TOKEN = token;
        B3_CHAIN_DOMAIN = chainDomain;
        ORIGIN_CHAIN_ID = chainId;
    }
}
