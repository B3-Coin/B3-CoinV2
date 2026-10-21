// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

interface IRebirthCustodyToken {
    function decimals() external view returns (uint8);
    function balanceOf(address account) external view returns (uint256);
    function transfer(address to, uint256 amount) external returns (bool);
    function transferFrom(address from, address to, uint256 amount) external returns (bool);
    function receiveWithAuthorization(address from, address to, uint256 value, uint256 validAfter,
        uint256 validBefore, bytes32 nonce, uint8 v, bytes32 r, bytes32 s) external;
}

/// Threshold custody for an explicitly pinned chain, token and deployment.
/// Three of four independent operators attest an already committed withdrawal.
/// The vault does not prove B3 consensus; that threshold is a custody trust
/// assumption. There is no owner, upgrade, arbitrary call or reserve sweep.
/// Deployment tooling in this release permits only an unforked chain31337.
contract RebirthCustodyVault {
    uint256 public constant QUORUM = 3;
    uint256 public constant MAX_ATOMS = uint256(1) << 62;
    uint256 private constant HALF_CURVE_ORDER =
        0x7fffffffffffffffffffffffffffffff5d576e7357a4501ddfe92f46681b20a0;
    bytes32 private constant DOMAIN_TYPEHASH =
        keccak256("EIP712Domain(string name,string version,uint256 chainId,address verifyingContract,bytes32 salt)");
    bytes32 private constant NAME_HASH = keccak256("B3 Rebirth Custody");
    bytes32 private constant VERSION_HASH = keccak256("2");
    bytes32 public constant WITHDRAWAL_TYPEHASH = keccak256(
        "Withdraw(bytes32 id,bytes32 account,bytes32 asset,uint256 amount,address recipient,uint64 slot,bytes32 stateRoot,uint64 validUntil,uint64 rosterEpoch)"
    );
    bytes32 public constant DEPOSIT_AUTHORIZATION_TYPEHASH = keccak256(
        "DepositAuthorization(uint256 chainId,address vault,bytes32 instance,bytes32 asset,address sender,uint256 amount,bytes32 account,bytes32 userSalt)"
    );
    bytes32 public constant PAUSE_TYPEHASH = keccak256("Pause(bool paused,uint64 nonce,uint64 rosterEpoch)");
    bytes32 public constant ROTATION_TYPEHASH =
        keccak256("Rotation(bytes32 nextRosterHash,uint64 nextEpoch,uint64 activateAt,uint64 nonce)");
    bytes32 public constant ACCEPT_ROTATION_TYPEHASH =
        keccak256("AcceptRotation(bytes32 nextRosterHash,uint64 nextEpoch,uint64 activateAt,uint64 nonce)");
    bytes32 public constant CANCEL_ROTATION_TYPEHASH =
        keccak256("CancelRotation(bytes32 rotationDigest,uint64 nonce,uint64 rosterEpoch)");

    struct Policy {
        uint256 chainId;
        uint64 disputeSeconds;
        uint64 rotationSeconds;
        uint64 maxCertificateLifetime;
    }

    struct DepositAuthorization {
        address sender;
        uint256 amount;
        bytes32 account;
        bytes32 userSalt;
        uint256 validAfter;
        uint256 validBefore;
        uint8 v;
        bytes32 r;
        bytes32 s;
    }

    address public immutable TOKEN;
    bytes32 public immutable TOKEN_CODE_HASH;
    bytes32 public immutable Q_ASSET_ID;
    bytes32 public immutable INSTANCE_ID;
    bytes32 public immutable POLICY_HASH;
    bytes32 public immutable DOMAIN_SEPARATOR;
    uint256 public immutable DEPLOYMENT_CHAIN_ID;
    uint64 public immutable DISPUTE_SECONDS;
    uint64 public immutable ROTATION_SECONDS;
    uint64 public immutable MAX_CERTIFICATE_LIFETIME;
    bytes32 public ROSTER_HASH;
    uint64 public rosterEpoch;
    address[4] public operators;
    mapping(address => bool) public isOperator;
    uint64 public nextDepositId;
    uint64 public pauseNonce;
    uint64 public rotationNonce;
    bool public paused;
    uint256 public locked;
    uint256 public pendingTotal;
    uint256 private entered;

    struct Withdrawal {
        bytes32 id;
        bytes32 account;
        bytes32 asset;
        uint256 amount;
        address recipient;
        uint64 slot;
        bytes32 stateRoot;
        uint64 validUntil;
    }

    // Deliberately the same public getter ABI as the earlier integration vault.
    struct PendingWithdrawal {
        bytes32 digest;
        address recipient;
        uint256 amount;
        uint64 releaseAt;
        bool paid;
    }
    mapping(bytes32 => PendingWithdrawal) public withdrawals;

    struct Rotation {
        bytes32 digest;
        bytes32 nextRosterHash;
        uint64 nextEpoch;
        uint64 activateAt;
        uint64 nonce;
    }
    Rotation public pendingRotation;
    address[4] private nextOperators;

    error WrongChain();
    error TokenCodeChanged();
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
    error NotPaused();
    error BadPauseNonce();
    error BadRotation();
    error RotationWindowOpen();
    error TransferFailed();
    error NonExactTransfer();
    error Reentrancy();

    event Deposit(uint64 indexed depositId, bytes32 indexed account, bytes32 indexed asset, uint256 amount, address sender);
    event WithdrawalRequested(bytes32 indexed id, bytes32 indexed account, address indexed recipient,
        uint256 amount, uint64 slot, bytes32 stateRoot, uint64 releaseAt);
    event Withdrawn(bytes32 indexed id, address indexed recipient, uint256 amount);
    event PauseChanged(bool paused, uint64 nonce);
    event RotationScheduled(bytes32 indexed digest, bytes32 indexed nextRosterHash, uint64 nextEpoch,
        uint64 activateAt, uint64 nonce);
    event RotationCancelled(bytes32 indexed digest, uint64 nonce);
    event RosterRotated(bytes32 indexed previousRosterHash, bytes32 indexed rosterHash, uint64 epoch);

    modifier boundDeployment() {
        if (block.chainid != DEPLOYMENT_CHAIN_ID) revert WrongChain();
        if (TOKEN.codehash != TOKEN_CODE_HASH) revert TokenCodeChanged();
        _;
    }
    modifier nonReentrant() {
        if (entered != 0) revert Reentrancy();
        entered = 1;
        _;
        entered = 0;
    }

    constructor(address token, bytes32 qAssetId, bytes32 instanceId, address[4] memory roster, Policy memory policy) {
        if (block.chainid != policy.chainId || policy.chainId == 0) revert WrongChain();
        // Short delays are solely an isolated integration convenience. Other
        // deployments cannot silently reuse the thirty-second test settings.
        // Five minutes is a provisional product floor, not a demonstrated
        // security bound or a promise about source settlement/finality time.
        uint64 minimumDispute = policy.chainId == 31337 ? 30 : uint64(300);
        uint64 minimumRotation = policy.chainId == 31337 ? 60 : uint64(7 days);
        if (token.code.length == 0 || qAssetId == bytes32(0) || instanceId == bytes32(0)
            || policy.disputeSeconds < minimumDispute || policy.disputeSeconds > 7 days
            || policy.rotationSeconds < minimumRotation || policy.rotationSeconds > 30 days
            || policy.maxCertificateLifetime == 0 || policy.maxCertificateLifetime > 1 days
            || IRebirthCustodyToken(token).decimals() != 6) revert BadConfiguration();
        _checkRoster(roster);
        for (uint256 i; i < 4; ++i) { operators[i] = roster[i]; isOperator[roster[i]] = true; }
        TOKEN = token;
        TOKEN_CODE_HASH = token.codehash;
        Q_ASSET_ID = qAssetId;
        INSTANCE_ID = instanceId;
        DEPLOYMENT_CHAIN_ID = policy.chainId;
        DISPUTE_SECONDS = policy.disputeSeconds;
        ROTATION_SECONDS = policy.rotationSeconds;
        MAX_CERTIFICATE_LIFETIME = policy.maxCertificateLifetime;
        ROSTER_HASH = keccak256(abi.encode(roster));
        POLICY_HASH = keccak256(abi.encode(policy.chainId, token, token.codehash, qAssetId, instanceId,
            policy.disputeSeconds, policy.rotationSeconds, policy.maxCertificateLifetime, ROSTER_HASH));
        bytes32 salt = keccak256(abi.encode(instanceId, token, qAssetId, token.codehash, POLICY_HASH));
        DOMAIN_SEPARATOR = keccak256(abi.encode(DOMAIN_TYPEHASH, NAME_HASH, VERSION_HASH,
            policy.chainId, address(this), salt));
    }

    function deposit(uint256 amount, bytes32 account) external boundDeployment nonReentrant {
        if (paused) revert Paused();
        if (amount == 0 || amount > MAX_ATOMS || account == bytes32(0)
            || nextDepositId == type(uint64).max || locked > MAX_ATOMS - amount) revert InvalidDeposit();
        uint256 beforeBalance = IRebirthCustodyToken(TOKEN).balanceOf(address(this));
        uint256 beforeSender = IRebirthCustodyToken(TOKEN).balanceOf(msg.sender);
        if (beforeBalance < locked) revert InsufficientReserve();
        _safeCall(abi.encodeCall(IRebirthCustodyToken.transferFrom, (msg.sender, address(this), amount)));
        uint256 afterBalance = IRebirthCustodyToken(TOKEN).balanceOf(address(this));
        uint256 afterSender = IRebirthCustodyToken(TOKEN).balanceOf(msg.sender);
        if (afterBalance < beforeBalance || afterBalance - beforeBalance != amount
            || afterSender > beforeSender || beforeSender - afterSender != amount) revert NonExactTransfer();
        locked += amount;
        emit Deposit(nextDepositId++, account, Q_ASSET_ID, amount, msg.sender);
    }

    /// The token's EIP-3009 nonce also commits the destination B3 account.
    /// A gas sponsor cannot redirect credit, substitute principal or reuse an
    /// authorization at another vault/chain/instance. The token owns replay
    /// protection for this exact sender/nonce. Wallets must display these fields.
    function depositAuthorizationNonce(address sender, uint256 amount, bytes32 account, bytes32 userSalt)
        public view boundDeployment returns (bytes32)
    {
        return keccak256(abi.encode(DEPOSIT_AUTHORIZATION_TYPEHASH, DEPLOYMENT_CHAIN_ID, address(this),
            INSTANCE_ID, Q_ASSET_ID, sender, amount, account, userSalt));
    }

    /// Anyone can sponsor gas for an already signed exact token authorization.
    /// No service fee is invented here: sponsorship pricing/funding is a
    /// separately qualified operator policy. This call never takes native ETH.
    function depositWithAuthorization(address sender, uint256 amount, bytes32 account, bytes32 userSalt,
        uint256 validAfter, uint256 validBefore, uint8 v, bytes32 r, bytes32 s)
        external boundDeployment nonReentrant
    {
        // Keep the external ABI flat for wallets, but keep the accounting and
        // nine-word token ABI encoder out of the external argument stack.
        DepositAuthorization memory a;
        a.sender = sender; a.amount = amount; a.account = account; a.userSalt = userSalt;
        a.validAfter = validAfter; a.validBefore = validBefore; a.v = v; a.r = r; a.s = s;
        _depositAuthorized(a);
    }

    function _depositAuthorized(DepositAuthorization memory a) private {
        if (paused) revert Paused();
        if (a.sender == address(0) || a.sender == address(this) || a.amount == 0 || a.amount > MAX_ATOMS
            || a.account == bytes32(0) || nextDepositId == type(uint64).max || locked > MAX_ATOMS - a.amount)
            revert InvalidDeposit();
        bytes32 nonce = depositAuthorizationNonce(a.sender, a.amount, a.account, a.userSalt);
        bytes memory transferCall = abi.encodeCall(IRebirthCustodyToken.receiveWithAuthorization,
            (a.sender, address(this), a.amount, a.validAfter, a.validBefore, nonce, a.v, a.r, a.s));
        uint256 beforeBalance = IRebirthCustodyToken(TOKEN).balanceOf(address(this));
        uint256 beforeSender = IRebirthCustodyToken(TOKEN).balanceOf(a.sender);
        if (beforeBalance < locked) revert InsufficientReserve();
        _safeCall(transferCall);
        uint256 afterBalance = IRebirthCustodyToken(TOKEN).balanceOf(address(this));
        uint256 afterSender = IRebirthCustodyToken(TOKEN).balanceOf(a.sender);
        if (afterBalance < beforeBalance || afterBalance - beforeBalance != a.amount
            || afterSender > beforeSender || beforeSender - afterSender != a.amount) revert NonExactTransfer();
        locked += a.amount;
        emit Deposit(nextDepositId++, a.account, Q_ASSET_ID, a.amount, a.sender);
    }

    function withdrawalDigest(Withdrawal calldata w) public view boundDeployment returns (bytes32) {
        return _typed(keccak256(abi.encode(WITHDRAWAL_TYPEHASH, w.id, w.account, w.asset, w.amount,
            w.recipient, w.slot, w.stateRoot, w.validUntil, rosterEpoch)));
    }

    function requestWithdrawal(Withdrawal calldata w, bytes[] calldata signatures) external boundDeployment nonReentrant {
        if (paused) revert Paused();
        if (w.id == bytes32(0) || w.account == bytes32(0) || w.asset != Q_ASSET_ID || w.amount == 0
            || w.amount > MAX_ATOMS || w.recipient == address(0) || w.recipient == address(this)
            || w.slot == 0 || w.stateRoot == bytes32(0)) revert InvalidWithdrawal();
        if (block.timestamp > w.validUntil) revert ExpiredCertificate();
        if (w.validUntil - block.timestamp > MAX_CERTIFICATE_LIFETIME) revert InvalidWithdrawal();
        if (withdrawals[w.id].digest != bytes32(0)) revert AlreadyRequested();
        if (pendingTotal > locked || w.amount > locked - pendingTotal
            || IRebirthCustodyToken(TOKEN).balanceOf(address(this)) < locked) revert InsufficientReserve();
        bytes32 digest = withdrawalDigest(w);
        _requireCurrentQuorum(digest, signatures);
        if (block.timestamp > type(uint64).max - DISPUTE_SECONDS) revert InvalidWithdrawal();
        uint64 releaseAt = uint64(block.timestamp) + DISPUTE_SECONDS;
        withdrawals[w.id] = PendingWithdrawal(digest, w.recipient, w.amount, releaseAt, false);
        pendingTotal += w.amount;
        emit WithdrawalRequested(w.id, w.account, w.recipient, w.amount, w.slot, w.stateRoot, releaseAt);
    }

    /// Once accepted, a claim survives expiry and roster rotation. Only its
    /// exact amount and recipient can be paid, once. Pause preserves the claim.
    function finalizeWithdrawal(bytes32 id) external boundDeployment nonReentrant {
        if (paused) revert Paused();
        PendingWithdrawal storage w = withdrawals[id];
        if (w.digest == bytes32(0)) revert UnknownWithdrawal();
        if (w.paid) revert AlreadyPaid();
        if (block.timestamp < w.releaseAt) revert DisputeWindowOpen();
        uint256 beforeVault = IRebirthCustodyToken(TOKEN).balanceOf(address(this));
        uint256 beforeRecipient = IRebirthCustodyToken(TOKEN).balanceOf(w.recipient);
        if (beforeVault < locked || w.amount > pendingTotal || w.amount > locked) revert InsufficientReserve();
        w.paid = true;
        pendingTotal -= w.amount;
        locked -= w.amount;
        _safeCall(abi.encodeCall(IRebirthCustodyToken.transfer, (w.recipient, w.amount)));
        uint256 afterVault = IRebirthCustodyToken(TOKEN).balanceOf(address(this));
        uint256 afterRecipient = IRebirthCustodyToken(TOKEN).balanceOf(w.recipient);
        if (afterVault > beforeVault || beforeVault - afterVault != w.amount
            || afterRecipient < beforeRecipient || afterRecipient - beforeRecipient != w.amount) revert NonExactTransfer();
        emit Withdrawn(id, w.recipient, w.amount);
    }

    function pauseDigest(bool pause, uint64 nonce) public view boundDeployment returns (bytes32) {
        return _typed(keccak256(abi.encode(PAUSE_TYPEHASH, pause, nonce, rosterEpoch)));
    }

    /// One bounded read lets each independent observer check the entire public
    /// identity at its canonical source checkpoint before any new signature.
    function custodyIdentity() external view boundDeployment returns (
        uint256 chainId, bytes32 tokenCodeHash, bytes32 policyHash, bytes32 domain,
        bytes32 rosterHash, uint64 epoch, uint64 dispute, uint64 rotation, uint64 lifetime,
        address token, bytes32 asset, bytes32 instance, bool isPaused, bytes32 scheduledRotation
    ) {
        return (DEPLOYMENT_CHAIN_ID, TOKEN_CODE_HASH, POLICY_HASH, DOMAIN_SEPARATOR, ROSTER_HASH,
            rosterEpoch, DISPUTE_SECONDS, ROTATION_SECONDS, MAX_CERTIFICATE_LIFETIME,
            TOKEN, Q_ASSET_ID, INSTANCE_ID, paused, pendingRotation.digest);
    }
    function setPaused(bool pause, uint64 nonce, bytes[] calldata signatures) external boundDeployment nonReentrant {
        if (pauseNonce == type(uint64).max || nonce != pauseNonce + 1) revert BadPauseNonce();
        _requireCurrentQuorum(pauseDigest(pause, nonce), signatures);
        pauseNonce = nonce;
        paused = pause;
        emit PauseChanged(pause, nonce);
    }

    function rotationDigest(address[4] calldata roster, uint64 activateAt, uint64 nonce)
        public view boundDeployment returns (bytes32)
    {
        if (rosterEpoch == type(uint64).max) revert BadRotation();
        return _typed(keccak256(abi.encode(ROTATION_TYPEHASH, keccak256(abi.encode(roster)),
            rosterEpoch + 1, activateAt, nonce)));
    }
    function acceptanceDigest(address[4] calldata roster, uint64 activateAt, uint64 nonce)
        public view boundDeployment returns (bytes32)
    {
        if (rosterEpoch == type(uint64).max) revert BadRotation();
        return _typed(keccak256(abi.encode(ACCEPT_ROTATION_TYPEHASH, keccak256(abi.encode(roster)),
            rosterEpoch + 1, activateAt, nonce)));
    }

    /// Both the outgoing and incoming three-of-four rosters approve the exact
    /// transition, while paused. It cannot activate before its immutable delay.
    function scheduleRotation(address[4] calldata roster, uint64 activateAt, uint64 nonce,
        bytes[] calldata outgoingSignatures, bytes[] calldata incomingSignatures) external boundDeployment nonReentrant
    {
        if (!paused) revert NotPaused();
        if (pendingRotation.digest != bytes32(0) || rotationNonce == type(uint64).max || nonce != rotationNonce + 1
            || block.timestamp > type(uint64).max - ROTATION_SECONDS
            || activateAt < block.timestamp + ROTATION_SECONDS || activateAt > block.timestamp + 30 days) revert BadRotation();
        _checkRoster(roster);
        bytes32 nextHash = keccak256(abi.encode(roster));
        if (nextHash == ROSTER_HASH) revert BadRotation();
        bytes32 digest = rotationDigest(roster, activateAt, nonce);
        _requireCurrentQuorum(digest, outgoingSignatures);
        _requireRosterQuorum(acceptanceDigest(roster, activateAt, nonce), incomingSignatures, roster);
        rotationNonce = nonce;
        pendingRotation = Rotation(digest, nextHash, rosterEpoch + 1, activateAt, nonce);
        nextOperators = roster;
        emit RotationScheduled(digest, nextHash, rosterEpoch + 1, activateAt, nonce);
    }
    function cancelRotationDigest() public view boundDeployment returns (bytes32) {
        Rotation memory pending = pendingRotation;
        if (pending.digest == bytes32(0)) revert BadRotation();
        return _typed(keccak256(abi.encode(CANCEL_ROTATION_TYPEHASH, pending.digest, pending.nonce, rosterEpoch)));
    }
    function cancelRotation(bytes[] calldata signatures) external boundDeployment nonReentrant {
        _requireCurrentQuorum(cancelRotationDigest(), signatures);
        bytes32 digest = pendingRotation.digest;
        delete pendingRotation;
        delete nextOperators;
        emit RotationCancelled(digest, rotationNonce);
    }
    function activateRotation() external boundDeployment nonReentrant {
        if (!paused) revert NotPaused();
        Rotation memory pending = pendingRotation;
        if (pending.digest == bytes32(0) || pending.nextEpoch != rosterEpoch + 1) revert BadRotation();
        if (block.timestamp < pending.activateAt) revert RotationWindowOpen();
        bytes32 previous = ROSTER_HASH;
        for (uint256 i; i < 4; ++i) isOperator[operators[i]] = false;
        for (uint256 i; i < 4; ++i) { operators[i] = nextOperators[i]; isOperator[operators[i]] = true; }
        rosterEpoch = pending.nextEpoch;
        ROSTER_HASH = pending.nextRosterHash;
        delete pendingRotation;
        delete nextOperators;
        emit RosterRotated(previous, ROSTER_HASH, rosterEpoch);
    }

    function _typed(bytes32 message) private view returns (bytes32) {
        return keccak256(abi.encodePacked(hex"1901", DOMAIN_SEPARATOR, message));
    }
    function _checkRoster(address[4] memory roster) private pure {
        address previous;
        for (uint256 i; i < 4; ++i) { if (roster[i] <= previous) revert BadConfiguration(); previous = roster[i]; }
    }
    function _recover(bytes32 digest, bytes calldata signature) private pure returns (address signer) {
        if (signature.length != 65) revert BadSignature();
        bytes32 r; bytes32 s; uint8 v;
        assembly {
            r := calldataload(signature.offset)
            s := calldataload(add(signature.offset, 32))
            v := byte(0, calldataload(add(signature.offset, 64)))
        }
        if ((v != 27 && v != 28) || uint256(s) == 0 || uint256(s) > HALF_CURVE_ORDER) revert BadSignature();
        signer = ecrecover(digest, v, r, s);
        if (signer == address(0)) revert BadSignature();
    }
    function _requireCurrentQuorum(bytes32 digest, bytes[] calldata signatures) private view {
        if (signatures.length < QUORUM || signatures.length > 4) revert BadQuorum();
        address previous;
        for (uint256 i; i < signatures.length; ++i) {
            address signer = _recover(digest, signatures[i]);
            if (!isOperator[signer] || signer <= previous) revert BadSignature();
            previous = signer;
        }
    }
    function _requireRosterQuorum(bytes32 digest, bytes[] calldata signatures, address[4] calldata roster) private pure {
        if (signatures.length < QUORUM || signatures.length > 4) revert BadQuorum();
        address previous;
        for (uint256 i; i < signatures.length; ++i) {
            address signer = _recover(digest, signatures[i]);
            bool member;
            for (uint256 j; j < 4; ++j) if (roster[j] == signer) member = true;
            if (!member || signer <= previous) revert BadSignature();
            previous = signer;
        }
    }
    function _safeCall(bytes memory data) private {
        (bool ok, bytes memory result) = TOKEN.call(data);
        if (!ok || (result.length != 0 && (result.length != 32 || !abi.decode(result, (bool))))) revert TransferFailed();
    }
}
