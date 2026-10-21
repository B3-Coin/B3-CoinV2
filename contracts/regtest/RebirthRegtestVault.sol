// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

interface IRebirthRegtestQ {
    function decimals() external view returns (uint8);
    function balanceOf(address account) external view returns (uint256);
    function transfer(address to, uint256 amount) external returns (bool);
    function transferFrom(address from, address to, uint256 amount) external returns (bool);
}

/// Local integration fixture only: fixed four-operator ECDSA roster, quorum
/// three, one synthetic six-decimal token and one immutable test instance.
/// The contract trusts that quorum to validate B3 inputs/state independently.
/// It does not prove the B3 chain, implement membership/view change or provide
/// production custody security. Every write is hard-gated to chain 31337.
/// Written independently; no Hyperliquid contract source is incorporated.
contract RebirthRegtestVault {
    uint256 public constant REGTEST_CHAIN_ID = 31337;
    uint256 public constant QUORUM = 3;
    uint256 public constant MAX_ATOMS = uint256(1) << 62;
    uint256 private constant HALF_CURVE_ORDER =
        0x7fffffffffffffffffffffffffffffff5d576e7357a4501ddfe92f46681b20a0;
    bytes32 private constant DOMAIN_TYPEHASH =
        keccak256("EIP712Domain(string name,string version,uint256 chainId,address verifyingContract,bytes32 salt)");
    bytes32 private constant NAME_HASH = keccak256("B3 Rebirth Regtest Custody");
    bytes32 private constant VERSION_HASH = keccak256("1");
    bytes32 public constant WITHDRAWAL_TYPEHASH = keccak256(
        "Withdraw(bytes32 id,bytes32 account,bytes32 asset,uint256 amount,address recipient,uint64 slot,bytes32 stateRoot,uint64 validUntil)"
    );
    bytes32 public constant PAUSE_TYPEHASH = keccak256("Pause(bool paused,uint64 nonce)");

    address public immutable TOKEN;
    bytes32 public immutable Q_ASSET_ID;
    bytes32 public immutable INSTANCE_ID;
    bytes32 public immutable ROSTER_HASH;
    bytes32 public immutable DOMAIN_SEPARATOR;
    uint64 public immutable DISPUTE_SECONDS;
    address[4] public operators;
    mapping(address => bool) public isOperator;

    uint64 public nextDepositId;
    uint64 public pauseNonce;
    bool public paused;
    uint256 public locked;
    uint256 public pendingTotal;
    uint256 private entered;

    struct Withdrawal {
        bytes32 id; // exact FlowMesh ActionId, raw bytes
        bytes32 account;
        bytes32 asset;
        uint256 amount; // external payout only; Q service fee stays in the ledger
        address recipient;
        uint64 slot;
        bytes32 stateRoot;
        uint64 validUntil; // latest acceptance time, not a queued claim's payout deadline
    }

    struct PendingWithdrawal {
        bytes32 digest;
        address recipient;
        uint256 amount;
        uint64 releaseAt;
        bool paid;
    }

    mapping(bytes32 => PendingWithdrawal) public withdrawals;

    error NotRegtest();
    error BadConfiguration();
    error BadQuorum();
    error BadSignature();
    error InvalidWithdrawal();
    error ExpiredCertificate();
    error InvalidDeposit();
    error InsufficientReserve();
    error AlreadyRequested();
    error UnknownWithdrawal();
    error AlreadyPaid();
    error DisputeWindowOpen();
    error Paused();
    error BadPauseNonce();
    error TransferFailed();
    error Reentrancy();

    event Deposit(
        uint64 indexed depositId,
        bytes32 indexed account,
        bytes32 indexed asset,
        uint256 amount,
        address sender
    );
    event WithdrawalRequested(
        bytes32 indexed id,
        bytes32 indexed account,
        address indexed recipient,
        uint256 amount,
        uint64 slot,
        bytes32 stateRoot,
        uint64 releaseAt
    );
    event Withdrawn(bytes32 indexed id, address indexed recipient, uint256 amount);
    event PauseChanged(bool paused, uint64 nonce);

    modifier onlyRegtest() {
        if (block.chainid != REGTEST_CHAIN_ID) revert NotRegtest();
        _;
    }

    modifier nonReentrant() {
        if (entered != 0) revert Reentrancy();
        entered = 1;
        _;
        entered = 0;
    }

    constructor(address token, bytes32 qAssetId, bytes32 instanceId, address[4] memory roster, uint64 disputeSeconds)
        onlyRegtest
    {
        if (
            token.code.length == 0 || qAssetId == bytes32(0) || instanceId == bytes32(0)
                || disputeSeconds == 0 || disputeSeconds > 1 hours || IRebirthRegtestQ(token).decimals() != 6
        ) revert BadConfiguration();
        address previous;
        for (uint256 i; i < 4; ++i) {
            if (roster[i] <= previous) revert BadConfiguration();
            previous = roster[i];
            operators[i] = roster[i];
            isOperator[roster[i]] = true;
        }
        TOKEN = token;
        Q_ASSET_ID = qAssetId;
        INSTANCE_ID = instanceId;
        DISPUTE_SECONDS = disputeSeconds;
        ROSTER_HASH = keccak256(abi.encode(roster));
        bytes32 salt = keccak256(abi.encode(instanceId, token, qAssetId, ROSTER_HASH));
        DOMAIN_SEPARATOR = keccak256(
            abi.encode(DOMAIN_TYPEHASH, NAME_HASH, VERSION_HASH, block.chainid, address(this), salt)
        );
    }

    /// Emits the exact received Q amount. Direct token transfers create no
    /// Deposit event and therefore grant no off-chain credit or release reserve.
    function deposit(uint256 amount, bytes32 account) external onlyRegtest nonReentrant {
        if (paused) revert Paused();
        if (amount == 0 || amount > MAX_ATOMS || account == bytes32(0)) revert InvalidDeposit();
        uint256 beforeBalance = IRebirthRegtestQ(TOKEN).balanceOf(address(this));
        _safeCall(abi.encodeCall(IRebirthRegtestQ.transferFrom, (msg.sender, address(this), amount)));
        uint256 received = IRebirthRegtestQ(TOKEN).balanceOf(address(this)) - beforeBalance;
        if (received != amount || locked > MAX_ATOMS - received) revert InvalidDeposit();
        locked += received;
        emit Deposit(nextDepositId++, account, Q_ASSET_ID, received, msg.sender);
    }

    /// EIP-712 typed digest; signatures are raw secp256k1 signatures of this
    /// digest (not personal_sign). Domain binds chain, vault, token, Q, instance
    /// and the full static roster. Every withdrawal field is authenticated.
    function withdrawalDigest(Withdrawal calldata w) public view onlyRegtest returns (bytes32) {
        bytes32 message = keccak256(
            abi.encode(
                WITHDRAWAL_TYPEHASH, w.id, w.account, w.asset, w.amount, w.recipient, w.slot, w.stateRoot, w.validUntil
            )
        );
        return keccak256(abi.encodePacked(hex"1901", DOMAIN_SEPARATOR, message));
    }

    function requestWithdrawal(Withdrawal calldata w, bytes[] calldata signatures) external onlyRegtest nonReentrant {
        if (paused) revert Paused();
        if (
            w.id == bytes32(0) || w.account == bytes32(0) || w.asset != Q_ASSET_ID || w.amount == 0
                || w.amount > MAX_ATOMS || w.recipient == address(0) || w.slot == 0 || w.stateRoot == bytes32(0)
        ) revert InvalidWithdrawal();
        if (block.timestamp > w.validUntil) revert ExpiredCertificate();
        if (withdrawals[w.id].digest != bytes32(0)) revert AlreadyRequested();
        if (w.amount > locked - pendingTotal) revert InsufficientReserve();
        bytes32 digest = withdrawalDigest(w);
        _requireQuorum(digest, signatures);
        if (block.timestamp > type(uint64).max - DISPUTE_SECONDS) revert InvalidWithdrawal();
        uint64 releaseAt = uint64(block.timestamp) + DISPUTE_SECONDS;
        withdrawals[w.id] = PendingWithdrawal(digest, w.recipient, w.amount, releaseAt, false);
        pendingTotal += w.amount;
        emit WithdrawalRequested(w.id, w.account, w.recipient, w.amount, w.slot, w.stateRoot, releaseAt);
    }

    /// Anyone may relay a queued claim. Its acceptance expiry does not erase
    /// an already accepted payment right. A token failure reverts all effects;
    /// the same id stays pending and can be retried after the fault is repaired.
    function finalizeWithdrawal(bytes32 id) external onlyRegtest nonReentrant {
        if (paused) revert Paused();
        PendingWithdrawal storage w = withdrawals[id];
        if (w.digest == bytes32(0)) revert UnknownWithdrawal();
        if (w.paid) revert AlreadyPaid();
        if (block.timestamp < w.releaseAt) revert DisputeWindowOpen();
        w.paid = true;
        pendingTotal -= w.amount;
        locked -= w.amount;
        _safeCall(abi.encodeCall(IRebirthRegtestQ.transfer, (w.recipient, w.amount)));
        emit Withdrawn(id, w.recipient, w.amount);
    }

    function pauseDigest(bool pause, uint64 nonce) public view onlyRegtest returns (bytes32) {
        return keccak256(abi.encodePacked(hex"1901", DOMAIN_SEPARATOR, keccak256(abi.encode(PAUSE_TYPEHASH, pause, nonce))));
    }

    /// Quorum-authorized pause is the fixture's dispute intervention. It stops
    /// deposits/new claims/payments without deleting accepted claims. No single
    /// owner can release funds or unpause. Static membership is intentional.
    function setPaused(bool pause, uint64 nonce, bytes[] calldata signatures) external onlyRegtest nonReentrant {
        if (pauseNonce == type(uint64).max || nonce != pauseNonce + 1) revert BadPauseNonce();
        _requireQuorum(pauseDigest(pause, nonce), signatures);
        pauseNonce = nonce;
        paused = pause;
        emit PauseChanged(pause, nonce);
    }

    function _requireQuorum(bytes32 digest, bytes[] calldata signatures) private view {
        if (signatures.length < QUORUM || signatures.length > 4) revert BadQuorum();
        address previous;
        for (uint256 i; i < signatures.length; ++i) {
            bytes calldata signature = signatures[i];
            if (signature.length != 65) revert BadSignature();
            bytes32 r;
            bytes32 s;
            uint8 v;
            assembly {
                r := calldataload(signature.offset)
                s := calldataload(add(signature.offset, 32))
                v := byte(0, calldataload(add(signature.offset, 64)))
            }
            if ((v != 27 && v != 28) || uint256(s) == 0 || uint256(s) > HALF_CURVE_ORDER) revert BadSignature();
            address signer = ecrecover(digest, v, r, s);
            // Strictly ascending recovered addresses enforces uniqueness and
            // bounds verification work to four independently registered keys.
            if (!isOperator[signer] || signer <= previous) revert BadSignature();
            previous = signer;
        }
    }

    function _safeCall(bytes memory data) private {
        (bool ok, bytes memory result) = TOKEN.call(data);
        if (!ok || (result.length != 0 && (result.length != 32 || !abi.decode(result, (bool))))) revert TransferFailed();
    }
}
