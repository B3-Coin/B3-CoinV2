// SPDX-License-Identifier: MIT
pragma solidity ^0.8.28;

import {B3Types} from "../src/IB3FinalityProver.sol";
import {B3TypesV2} from "../src/v2/B3TypesV2.sol";
import {BlsDigestProver} from "../src/v2/BlsDigestProver.sol";
import {B3FinalityVerifierV2} from "../src/v2/B3FinalityVerifierV2.sol";
import {B3VaultV2} from "../src/v2/B3VaultV2.sol";
import {B3RelayBatch} from "../src/v2/B3RelayBatch.sol";
import {B3Forwarder} from "../src/v2/B3Forwarder.sol";
import {B3DepositForwarderFactory} from "../src/v2/B3DepositForwarderFactory.sol";
import {B3OftExitFactory} from "../src/v2/B3OftExitFactory.sol";
import {IOFTV2} from "../src/v2/IOFTV2.sol";

/// Foundry cheat codes used by DeployV2. Declared locally, like V1's
/// ScriptBase.sol (which stays untouched), so the script needs neither
/// forge-std nor a network install. Signatures follow Foundry 1.7.1.
interface ScriptVmV2 {
    function envAddress(string calldata name) external view returns (address value);
    function envBytes(string calldata name) external view returns (bytes memory value);
    function envBytes32(string calldata name) external view returns (bytes32 value);
    function envUint(string calldata name) external view returns (uint256 value);
    function envOr(string calldata name, uint256 defaultValue) external view returns (uint256 value);
    function envOr(string calldata name, address defaultValue) external view returns (address value);
    function envOr(string calldata name, bytes32 defaultValue) external view returns (bytes32 value);
    function envOr(string calldata name, string calldata defaultValue) external view returns (string memory value);
    function envOr(string calldata name, bytes calldata defaultValue) external view returns (bytes memory value);
    function envOr(string calldata name, string calldata delim, address[] calldata defaultValue)
        external
        view
        returns (address[] memory value);
    function startBroadcast(address signer) external;
    function stopBroadcast() external;
    function getNonce(address account) external view returns (uint64 nonce);
    function computeCreateAddress(address deployer, uint256 nonce) external pure returns (address);
    function serializeAddress(string calldata objectKey, string calldata valueKey, address value)
        external
        returns (string memory json);
    function serializeAddress(string calldata objectKey, string calldata valueKey, address[] calldata values)
        external
        returns (string memory json);
    function serializeBool(string calldata objectKey, string calldata valueKey, bool value)
        external
        returns (string memory json);
    function serializeBytes(string calldata objectKey, string calldata valueKey, bytes calldata value)
        external
        returns (string memory json);
    function serializeBytes32(string calldata objectKey, string calldata valueKey, bytes32 value)
        external
        returns (string memory json);
    function serializeString(string calldata objectKey, string calldata valueKey, string calldata value)
        external
        returns (string memory json);
    function serializeUint(string calldata objectKey, string calldata valueKey, uint256 value)
        external
        returns (string memory json);
    function writeJson(string calldata json, string calldata path) external;
}

/// Deploys the bridge V2 launch stack (doc/design/b3-bridge-v2-contracts-spec.md
/// §3-§9) in dependency order: BlsDigestProver -> B3FinalityVerifierV2 (pinned
/// to the prover's code hash) -> B3VaultV2 (pinned to the verifier's code
/// hash) -> B3RelayBatch. All four are CREATEs by B3V2_DEPLOYER at nonces
/// n .. n+3, so the vault's forwarder factory defaults to the deployer's next
/// CREATE address (nonce n+4), the F_d factory's slot.
///
/// Forwarder factories (spec §7-§8; routes (b) and (d3)): with
/// B3V2_DEPLOY_FORWARDERS=1 the same broadcast continues with
/// B3DepositForwarderFactory(vault, vault code hash) at nonce n+4, the
/// vault's predicted factory, and B3OftExitFactory(origin token,
/// B3V2_OFT_ADAPTER, B3V2_EXIT_DST_EID, B3V2_EXIT_EXTRA_OPTIONS) at n+5.
/// Both derive accounts with keccak256(type(B3Forwarder).creationCode), which
/// the manifest records (forwarder_init_code_hash) for B3's
/// deposit_forwarder / exit_forwarder pins. The default (0) keeps the four
/// CREATEs, so earlier harnesses keep their nonces.
///
/// Latent deployer privilege: until the reviewed F_d factory is deployed at
/// nonce n+4, whatever B3V2_DEPLOYER deploys there answers the vault's
/// isAccount() caller check (C-13). A factory answering true for every
/// address would let contract wallets deposit receipts B3 cannot prove
/// (unmintable deposits); a nonce n+4 spent on any other transaction loses
/// the slot for good. The vault only staticcalls the factory, so no theft
/// path exists. B3V2_DEPLOY_FORWARDERS=1 closes the window in the same
/// reviewed sequence. Regtest only; before any non-regtest deployment (S7)
/// the factory must be deployed that way and its code hash pinned in the
/// manifest and by B3. The manifest records the state
/// (forwarder_factory_code_hash, forwarder_factory_latent_deployer_privilege).
///
/// REGTEST ONLY in stage S3: run() and deploy() refuse every chain id but
/// 31337, and the defaults are regtest profile R (regtest plan §3.3: B-10
/// scaled by 1/36). Mainnet deployment waits for stage S7 and is not
/// authorized; validateWindows() and validateMainnetPins() only encode the
/// chain-1 policy (spec §3.1, §4, §5; C-9, C-14) for that stage and are
/// exercised by unit tests alone. The script imports no regtest mock.
///
/// Environment (integers are decimal or 0x-hex):
/// - Required: B3V2_DEPLOYER, B3V2_CHAIN_DOMAIN, B3V2_GENESIS_HEIGHT,
///   B3V2_GENESIS_BLOCK_HASH, B3V2_GENESIS_SET_HASH, B3V2_START_EPOCH,
///   B3V2_START_SET_HASH, B3V2_BOOTSTRAP_MODE (0 K_OF_N, 1 START_CERTIFICATE),
///   B3V2_BRIDGE_ACTIVATION_HEIGHT, B3V2_ORIGIN_TOKEN.
/// - K_OF_N only: B3V2_BOOTSTRAP_AGGREGATE_PUBKEY (48 bytes),
///   B3V2_BOOTSTRAP_MEMBERS_ROOT, B3V2_BOOTSTRAP_SET_HASH (the 4-member,
///   weight-4, quorum-3 ruleset-2 header they form must hash to it).
///   START_CERTIFICATE refuses a non-zero B3V2_BOOTSTRAP_SET_HASH.
/// - Optional: B3V2_LINEAGE_ID (cross-check; default recomputed),
///   B3V2_MIN_BRIDGE_VALIDATORS (4), B3V2_MAX_BRIDGE_VALIDATORS (64),
///   B3V2_MIN_BRIDGE_TOTAL_WEIGHT (900), B3V2_MIN_EPOCH_DURATION (600),
///   B3V2_MAX_EPOCH_LAG (72000), B3V2_DEPOSIT_CLOSE_AFTER (7200),
///   B3V2_MAX_CERTIFICATE_AGE (2400), B3V2_INIT_WINDOW (16800; INIT_DEADLINE
///   = now + window), B3V2_REVIEW_WINDOW (16800), B3V2_WATCHERS (comma list,
///   default none), B3V2_TOKEN_CODE_HASH (default the token's code hash),
///   B3V2_MAX_DEPOSIT_RAW (10,000,000,000), B3V2_FORWARDER_FACTORY (default
///   the predicted nonce-n+4 address), B3V2_MANIFEST_PATH (optional; must be
///   deployments/local-<name>.json, which .gitignore covers).
/// - Forwarders: B3V2_DEPLOY_FORWARDERS (0 or 1; default 0). With 1:
///   B3V2_OFT_ADAPTER (required; code, token() == B3V2_ORIGIN_TOKEN),
///   B3V2_EXIT_DST_EID (30110, Arbitrum One), B3V2_EXIT_EXTRA_OPTIONS
///   (default empty), and B3V2_FORWARDER_FACTORY must stay unset. With 0,
///   a set B3V2_OFT_ADAPTER or B3V2_EXIT_EXTRA_OPTIONS is refused as a
///   mixed-up environment.
///
/// The manifest records addresses, code hashes, the AssetId, every pin and
/// window and the forwarder factories (zero when not deployed). It omits the
/// deployment block, which the harness takes from receipts or the broadcast
/// output. Forge writes it during its simulation, before broadcasting, so it
/// stays a candidate ("regtest_candidate") until the harness sees code at the
/// four (six with forwarders) addresses.
contract DeployV2 {
    ScriptVmV2 internal constant vm = ScriptVmV2(address(uint160(uint256(keccak256("hevm cheat code")))));

    struct Windows {
        uint256 minEpochDuration;
        uint256 maxEpochLag;
        uint256 depositCloseAfter;
        uint256 maxCertificateAge;
        uint256 initWindow; // INIT_DEADLINE = deployment time + initWindow
        uint256 reviewWindow;
    }

    /// Deployment inputs (the environment, before defaults that need chain state).
    struct Params {
        address deployer;
        bytes32 chainDomain;
        uint64 genesisHeight;
        bytes32 genesisBlockHash;
        bytes32 genesisSetHash;
        bytes32 lineageId;
        uint64 startEpoch;
        bytes32 startSetHash;
        B3FinalityVerifierV2.BootstrapMode bootstrapMode;
        bytes bootstrapAggregatePubkey; // K_OF_N only
        bytes32 bootstrapMembersRoot; // K_OF_N only
        bytes32 bootstrapSetHash; // K_OF_N only; zero for START_CERTIFICATE
        uint64 bridgeActivationHeight;
        uint32 minBridgeValidators;
        uint32 maxBridgeValidators;
        uint64 minBridgeTotalWeight;
        Windows windows;
        address[] watchers;
        address originToken;
        bytes32 tokenCodeHash; // zero: the token's code hash at deployment
        uint256 maxDepositRaw;
        address forwarderFactory; // zero: the deployer's nonce-n+4 CREATE address
        string manifestPath; // empty: no manifest
        bool deployForwarders; // F_d factory at n+4 and F_x factory at n+5
        address oftAdapter; // forwarders only: the USDT0 OFT adapter
        uint32 exitDstEid; // forwarders only: the F_x destination endpoint id
        bytes exitExtraOptions; // forwarders only: F_x executor options
    }

    struct Deployment {
        BlsDigestProver prover;
        bytes32 proverCodeHash;
        B3FinalityVerifierV2 verifier;
        bytes32 verifierCodeHash;
        B3VaultV2 vault;
        bytes32 vaultCodeHash;
        B3RelayBatch batch;
        bytes32 batchCodeHash;
        bytes32 assetId;
        bytes32 tokenCodeHash;
        address forwarderFactory;
        bool forwarderFactoryPredicted;
        uint64 deployerNonce; // the deployer's nonce before the four CREATEs
        uint256 initDeadline;
        // Zero unless p.deployForwarders.
        B3DepositForwarderFactory depositFactory; // nonce n+4, == forwarderFactory
        bytes32 depositFactoryCodeHash;
        B3OftExitFactory exitFactory; // nonce n+5
        bytes32 exitFactoryCodeHash;
        bytes32 forwarderInitCodeHash; // keccak256(type(B3Forwarder).creationCode)
    }

    /// Chain-1 values for S7 (spec §3.1, §4, §5; C-9, C-14). Unit-tested only.
    struct MainnetPins {
        uint256 chainId;
        address originToken;
        uint32 minBridgeValidators;
        uint32 maxBridgeValidators;
        uint64 minBridgeTotalWeight;
        uint256 maxDepositRaw;
        uint256 minEpochDuration;
        uint256 maxEpochLag;
        uint256 depositCloseAfter;
        uint256 maxCertificateAge;
        uint256 reviewWindow;
        uint256 initDeadline;
        uint256 nowTimestamp;
        uint256 sMax; // slowest sustained mean B3 block spacing, seconds (O8, O2)
    }

    uint256 public constant REGTEST_CHAIN_ID = 31_337;
    uint256 public constant MAINNET_CHAIN_ID = 1;
    string public constant MANIFEST_PREFIX = "deployments/local-";
    string public constant MANIFEST_SUFFIX = ".json";
    string public constant MANIFEST_SCHEMA = "b3-bridge-v2-deployment/1";
    /// The forwarder factory is the deployer's CREATE right after the four contracts.
    uint256 public constant FORWARDER_FACTORY_NONCE_OFFSET = 4;
    /// With forwarders, the F_x factory is the CREATE after the F_d factory.
    uint256 public constant EXIT_FORWARDER_FACTORY_NONCE_OFFSET = 5;
    /// LayerZero endpoint id of Arbitrum One, F_x's destination (spec §8).
    uint32 public constant DEFAULT_EXIT_DST_EID = 30_110;
    /// The F_d factory's smallest flush (spec §7.3): the vault cap must admit
    /// it, and the deployed factory must report it.
    uint256 public constant FORWARDER_MIN_FLUSH_RAW = 1_000_000;

    // Regtest profile R defaults (regtest plan §3.3; OI-10).
    uint256 public constant R_MIN_EPOCH_DURATION = 600;
    uint256 public constant R_MAX_EPOCH_LAG = 72_000;
    uint256 public constant R_DEPOSIT_CLOSE_AFTER = 7_200;
    uint256 public constant R_MAX_CERTIFICATE_AGE = 2_400;
    uint256 public constant R_INIT_WINDOW = 16_800;
    uint256 public constant R_REVIEW_WINDOW = 16_800;

    // Bridge qualification and cap defaults (spec §3.1, V1 manifest; C-9, B-14).
    uint32 public constant DEFAULT_MIN_BRIDGE_VALIDATORS = 4;
    uint32 public constant DEFAULT_MAX_BRIDGE_VALIDATORS = 64;
    uint64 public constant DEFAULT_MIN_BRIDGE_TOTAL_WEIGHT = 900;
    uint256 public constant DEFAULT_MAX_DEPOSIT_RAW = 10_000_000_000;

    // Window rules on chain ids other than 31337, as in the verifier constructor (spec §4).
    uint256 public constant MIN_MAINNET_EPOCH_DURATION = 3_600;
    uint256 public constant MAINNET_LAG_MARGIN = 7 days;
    uint256 public constant MIN_MAINNET_REVIEW_WINDOW = 7 days;
    uint256 public constant MAX_WINDOW = type(uint40).max;

    // Chain-1 pins for S7 (spec §4 B-10, §3.1, §5; C-9, C-14).
    uint256 public constant B10_MIN_EPOCH_DURATION = 21_600;
    uint256 public constant B10_MAX_EPOCH_LAG = 2_592_000;
    uint256 public constant B10_DEPOSIT_CLOSE_AFTER = 259_200;
    uint256 public constant B10_MAX_CERTIFICATE_AGE = 86_400;
    uint256 public constant MAINNET_REVIEW_WINDOW = 7 days;
    uint256 public constant MAINNET_MAX_INIT_WINDOW = 7 days;
    address public constant MAINNET_USDT = 0xdAC17F958D2ee523a2206206994597C13D831ec7;
    /// DEPOSIT_CLOSE_AFTER >= 1,472 x S_MAX + 21,600 s: a 1,440-block epoch
    /// plus t_fin's 32 blocks, and 6 h of relay margin (spec §4).
    uint256 public constant CLOSE_EPOCH_BLOCKS = 1_472;
    uint256 public constant CLOSE_RELAY_MARGIN = 21_600;

    error NotRegtest(uint256 chainId);
    error IntegerOutOfRange(string name);
    error BadParam(string name);
    error LineageMismatch(bytes32 expected, bytes32 given);
    error BadWindows(string reason);
    error BadMainnetPin(string name);
    error BadManifestPath(string path);
    error PostCheckFailed(string what);

    // ===================================================================== run

    /// Reads the environment, deploys the four contracts (six with
    /// forwarders) as B3V2_DEPLOYER, checks every immutable and, if
    /// B3V2_MANIFEST_PATH is set, writes the manifest. Refuses every chain
    /// but 31337 before reading anything.
    function run() external returns (Deployment memory d) {
        _requireRegtest();
        Params memory p = readParams();
        bool manifest = bytes(p.manifestPath).length != 0;
        if (manifest) validateManifestPath(p.manifestPath);
        d = deploy(p);
        if (manifest) vm.writeJson(manifestJson(p, d), p.manifestPath);
    }

    /// The deployment inputs from the environment (see the contract comment).
    function readParams() public view returns (Params memory p) {
        p.deployer = vm.envAddress("B3V2_DEPLOYER");
        p.chainDomain = vm.envBytes32("B3V2_CHAIN_DOMAIN");
        p.genesisHeight = _envUint64("B3V2_GENESIS_HEIGHT");
        p.genesisBlockHash = vm.envBytes32("B3V2_GENESIS_BLOCK_HASH");
        p.genesisSetHash = vm.envBytes32("B3V2_GENESIS_SET_HASH");
        p.startEpoch = _envUint64("B3V2_START_EPOCH");
        p.startSetHash = vm.envBytes32("B3V2_START_SET_HASH");
        p.bootstrapMode = parseBootstrapMode(vm.envUint("B3V2_BOOTSTRAP_MODE"));
        if (p.bootstrapMode == B3FinalityVerifierV2.BootstrapMode.K_OF_N) {
            p.bootstrapAggregatePubkey = vm.envBytes("B3V2_BOOTSTRAP_AGGREGATE_PUBKEY");
            p.bootstrapMembersRoot = vm.envBytes32("B3V2_BOOTSTRAP_MEMBERS_ROOT");
            p.bootstrapSetHash = vm.envBytes32("B3V2_BOOTSTRAP_SET_HASH");
        } else if (vm.envOr("B3V2_BOOTSTRAP_SET_HASH", bytes32(0)) != bytes32(0)) {
            // START_CERTIFICATE has no bootstrap keys; a pinned hash means a mixed-up environment.
            revert BadParam("B3V2_BOOTSTRAP_SET_HASH");
        }
        p.bridgeActivationHeight = _envUint64("B3V2_BRIDGE_ACTIVATION_HEIGHT");
        p.originToken = vm.envAddress("B3V2_ORIGIN_TOKEN");

        p.lineageId = vm.envOr("B3V2_LINEAGE_ID", bytes32(0));
        if (p.lineageId == bytes32(0)) {
            p.lineageId = B3TypesV2.lineageId(p.chainDomain, p.genesisHeight, p.genesisBlockHash, p.genesisSetHash);
        }
        p.minBridgeValidators = _envOrUint32("B3V2_MIN_BRIDGE_VALIDATORS", DEFAULT_MIN_BRIDGE_VALIDATORS);
        p.maxBridgeValidators = _envOrUint32("B3V2_MAX_BRIDGE_VALIDATORS", DEFAULT_MAX_BRIDGE_VALIDATORS);
        p.minBridgeTotalWeight = _envOrUint64("B3V2_MIN_BRIDGE_TOTAL_WEIGHT", DEFAULT_MIN_BRIDGE_TOTAL_WEIGHT);
        p.windows = Windows({
            minEpochDuration: vm.envOr("B3V2_MIN_EPOCH_DURATION", R_MIN_EPOCH_DURATION),
            maxEpochLag: vm.envOr("B3V2_MAX_EPOCH_LAG", R_MAX_EPOCH_LAG),
            depositCloseAfter: vm.envOr("B3V2_DEPOSIT_CLOSE_AFTER", R_DEPOSIT_CLOSE_AFTER),
            maxCertificateAge: vm.envOr("B3V2_MAX_CERTIFICATE_AGE", R_MAX_CERTIFICATE_AGE),
            initWindow: vm.envOr("B3V2_INIT_WINDOW", R_INIT_WINDOW),
            reviewWindow: vm.envOr("B3V2_REVIEW_WINDOW", R_REVIEW_WINDOW)
        });
        p.watchers = vm.envOr("B3V2_WATCHERS", ",", new address[](0));
        p.tokenCodeHash = vm.envOr("B3V2_TOKEN_CODE_HASH", bytes32(0));
        p.maxDepositRaw = vm.envOr("B3V2_MAX_DEPOSIT_RAW", DEFAULT_MAX_DEPOSIT_RAW);
        p.forwarderFactory = vm.envOr("B3V2_FORWARDER_FACTORY", address(0));
        string memory none = "";
        p.manifestPath = vm.envOr("B3V2_MANIFEST_PATH", none);

        p.deployForwarders = parseDeployForwarders(vm.envOr("B3V2_DEPLOY_FORWARDERS", uint256(0)));
        p.oftAdapter = vm.envOr("B3V2_OFT_ADAPTER", address(0));
        p.exitDstEid = _envOrUint32("B3V2_EXIT_DST_EID", DEFAULT_EXIT_DST_EID);
        bytes memory noOptions = "";
        p.exitExtraOptions = vm.envOr("B3V2_EXIT_EXTRA_OPTIONS", noOptions);
    }

    /// Deploys and post-checks the stack for `p` (regtest only). Public so a
    /// test can deploy variants without touching the shared process environment.
    function deploy(Params memory p) public returns (Deployment memory d) {
        _requireRegtest();
        B3Types.SetHeader memory bootstrapSet = checkParams(p);

        d.tokenCodeHash = p.tokenCodeHash == bytes32(0) ? p.originToken.codehash : p.tokenCodeHash;
        if (p.originToken.codehash != d.tokenCodeHash) revert BadParam("B3V2_TOKEN_CODE_HASH");
        d.deployerNonce = vm.getNonce(p.deployer);
        d.forwarderFactoryPredicted = p.forwarderFactory == address(0);
        d.forwarderFactory = d.forwarderFactoryPredicted
            ? vm.computeCreateAddress(p.deployer, uint256(d.deployerNonce) + FORWARDER_FACTORY_NONCE_OFFSET)
            : p.forwarderFactory;
        d.initDeadline = block.timestamp + p.windows.initWindow;

        // Only the CREATEs (four, six with forwarders) are broadcast: code hashes
        // are read by opcode, and every external call (the post-checks) comes
        // after stopBroadcast. The factories' constructors read the vault and
        // the adapter inside their own CREATE.
        vm.startBroadcast(p.deployer);
        d.prover = new BlsDigestProver();
        d.proverCodeHash = address(d.prover).codehash;
        d.verifier = new B3FinalityVerifierV2(_verifierConfig(p, d), bootstrapSet, p.watchers);
        d.verifierCodeHash = address(d.verifier).codehash;
        d.vault = new B3VaultV2(
            B3VaultV2.Config({
                verifier: d.verifier,
                verifierCodeHash: d.verifierCodeHash,
                b3ChainDomain: p.chainDomain,
                originToken: p.originToken,
                tokenCodeHash: d.tokenCodeHash,
                forwarderFactory: d.forwarderFactory,
                maxDepositRaw: p.maxDepositRaw,
                predecessor: address(0),
                predecessorAssetId: bytes32(0)
            })
        );
        d.vaultCodeHash = address(d.vault).codehash;
        d.batch = new B3RelayBatch();
        d.batchCodeHash = address(d.batch).codehash;
        if (p.deployForwarders) {
            // Nonce n+4: the address the vault already names as FORWARDER_FACTORY.
            d.depositFactory = new B3DepositForwarderFactory(address(d.vault), d.vaultCodeHash);
            d.depositFactoryCodeHash = address(d.depositFactory).codehash;
            // Nonce n+5.
            d.exitFactory = new B3OftExitFactory(p.originToken, p.oftAdapter, p.exitDstEid, p.exitExtraOptions);
            d.exitFactoryCodeHash = address(d.exitFactory).codehash;
        }
        vm.stopBroadcast();
        if (p.deployForwarders) d.forwarderInitCodeHash = keccak256(type(B3Forwarder).creationCode);

        // forge-lint: disable-next-line(unsafe-typecast)
        d.assetId = B3TypesV2.assetId(p.chainDomain, uint64(block.chainid), address(d.vault), p.originToken);
        verifyDeployment(p, d);
    }

    // ================================================================== checks

    /// Pre-broadcast checks that give clearer errors than the constructors;
    /// returns the K_OF_N bootstrap header (empty for START_CERTIFICATE).
    function checkParams(Params memory p) public view returns (B3Types.SetHeader memory bootstrapSet) {
        if (p.deployer == address(0)) revert BadParam("B3V2_DEPLOYER");
        if (p.originToken.code.length == 0) revert BadParam("B3V2_ORIGIN_TOKEN");
        bytes32 expected = B3TypesV2.lineageId(p.chainDomain, p.genesisHeight, p.genesisBlockHash, p.genesisSetHash);
        if (p.lineageId != expected) revert LineageMismatch(expected, p.lineageId);
        validateWindows(block.chainid, p.windows);
        if (p.bootstrapMode == B3FinalityVerifierV2.BootstrapMode.K_OF_N) {
            if (p.bootstrapAggregatePubkey.length != 48) revert BadParam("B3V2_BOOTSTRAP_AGGREGATE_PUBKEY");
            if (p.bootstrapMembersRoot == bytes32(0)) revert BadParam("B3V2_BOOTSTRAP_MEMBERS_ROOT");
            bootstrapSet = bootstrapHeader(p.bootstrapAggregatePubkey, p.bootstrapMembersRoot);
            if (
                p.bootstrapSetHash == bytes32(0) || !B3TypesV2.headerShapeValid(bootstrapSet)
                    || B3Types.hashSetHeader(bootstrapSet) != p.bootstrapSetHash
            ) revert BadParam("B3V2_BOOTSTRAP_SET_HASH");
        } else if (p.bootstrapSetHash != bytes32(0)) {
            revert BadParam("B3V2_BOOTSTRAP_SET_HASH");
        }
        checkForwarderParams(p);
    }

    /// Forwarder inputs (spec §7.3, §8). With p.deployForwarders the F_d
    /// factory takes the predicted nonce-n+4 slot, so B3V2_FORWARDER_FACTORY
    /// must be unset; the adapter has code and its token() is the origin
    /// token; the destination endpoint id is non-zero; and the vault's cap
    /// admits a minimum flush. These are the factories' constructor rules,
    /// checked before anything is broadcast. Without forwarders a set adapter
    /// or option bytes means a mixed-up environment and is refused.
    function checkForwarderParams(Params memory p) public view {
        if (!p.deployForwarders) {
            if (p.oftAdapter != address(0)) revert BadParam("B3V2_OFT_ADAPTER");
            if (p.exitExtraOptions.length != 0) revert BadParam("B3V2_EXIT_EXTRA_OPTIONS");
            return;
        }
        if (p.forwarderFactory != address(0)) revert BadParam("B3V2_FORWARDER_FACTORY");
        if (p.oftAdapter.code.length == 0) revert BadParam("B3V2_OFT_ADAPTER");
        (bool ok, bytes memory data) = p.oftAdapter.staticcall(abi.encodeCall(IOFTV2.token, ()));
        if (!ok || data.length != 32 || abi.decode(data, (bytes32)) != bytes32(uint256(uint160(p.originToken)))) {
            revert BadParam("B3V2_OFT_ADAPTER");
        }
        if (p.exitDstEid == 0) revert BadParam("B3V2_EXIT_DST_EID");
        if (p.maxDepositRaw < FORWARDER_MIN_FLUSH_RAW) revert BadParam("B3V2_MAX_DEPOSIT_RAW");
    }

    /// B3V2_DEPLOY_FORWARDERS: 0 (default) or 1.
    function parseDeployForwarders(uint256 flag) public pure returns (bool) {
        if (flag > 1) revert BadParam("B3V2_DEPLOY_FORWARDERS");
        return flag == 1;
    }

    /// The K_OF_N bootstrap header (spec §3.1, §5): epoch 0, ruleset 2, four
    /// members of weight 1, quorum 3, so both prover quorums are exactly 3-of-4.
    function bootstrapHeader(bytes memory aggregatePubkey, bytes32 membersRoot)
        public
        pure
        returns (B3Types.SetHeader memory)
    {
        return B3Types.SetHeader({
            epoch: 0,
            rulesetVersion: B3TypesV2.RULESET_V2,
            validatorCount: 4,
            totalWeight: 4,
            quorumWeight: 3,
            aggregatePubkey: aggregatePubkey,
            membersRoot: membersRoot
        });
    }

    function parseBootstrapMode(uint256 mode) public pure returns (B3FinalityVerifierV2.BootstrapMode) {
        if (mode > uint256(B3FinalityVerifierV2.BootstrapMode.START_CERTIFICATE)) {
            revert BadParam("B3V2_BOOTSTRAP_MODE");
        }
        return B3FinalityVerifierV2.BootstrapMode(mode);
    }

    /// Window orderings per chain id, the verifier constructor's rules (spec
    /// §4): on 31337 any non-zero windows with 0 < age < close < lag and
    /// 0 < minEpoch <= lag (OI-10); on every other chain id minEpoch >= 1 h,
    /// lag >= close + 7 days and review >= 7 days, so relaxed (profile R)
    /// windows are refused. Both need 0 < review < lag and 0 < initWindow <= lag.
    function validateWindows(uint256 chainId, Windows memory w) public pure {
        if (w.maxEpochLag == 0 || w.maxEpochLag > MAX_WINDOW) revert BadWindows("MAX_EPOCH_LAG out of range");
        if (w.maxCertificateAge == 0 || w.maxCertificateAge >= w.depositCloseAfter) {
            revert BadWindows("need 0 < MAX_CERTIFICATE_AGE < DEPOSIT_CLOSE_AFTER");
        }
        if (w.minEpochDuration == 0 || w.minEpochDuration > w.maxEpochLag) {
            revert BadWindows("need 0 < MIN_EPOCH_DURATION <= MAX_EPOCH_LAG");
        }
        if (w.reviewWindow == 0 || w.reviewWindow >= w.maxEpochLag) {
            revert BadWindows("need 0 < REVIEW_WINDOW < MAX_EPOCH_LAG");
        }
        if (w.initWindow == 0 || w.initWindow > w.maxEpochLag) {
            revert BadWindows("need 0 < INIT_WINDOW <= MAX_EPOCH_LAG");
        }
        if (chainId == REGTEST_CHAIN_ID) {
            if (w.depositCloseAfter >= w.maxEpochLag) revert BadWindows("need DEPOSIT_CLOSE_AFTER < MAX_EPOCH_LAG");
        } else {
            if (w.minEpochDuration < MIN_MAINNET_EPOCH_DURATION) revert BadWindows("need MIN_EPOCH_DURATION >= 1 h");
            if (w.depositCloseAfter > w.maxEpochLag || w.maxEpochLag - w.depositCloseAfter < MAINNET_LAG_MARGIN) {
                revert BadWindows("need MAX_EPOCH_LAG >= DEPOSIT_CLOSE_AFTER + 7 days");
            }
            if (w.reviewWindow < MIN_MAINNET_REVIEW_WINDOW) revert BadWindows("need REVIEW_WINDOW >= 7 days");
        }
    }

    /// The chain-1 deployment policy for stage S7 (spec §3.1, §4 B-10, §5;
    /// C-9, C-14): exact B-10 windows, the 4/64/900 bridge bounds, the
    /// 10,000 USDT cap, Ethereum-mainnet USDT, a 7-day review, an init
    /// deadline within 7 days, and DEPOSIT_CLOSE_AFTER >= 1,472 x S_MAX +
    /// 21,600 s. Unit-tested only; nothing deploys to chain 1 in S3.
    function validateMainnetPins(MainnetPins memory m) public pure {
        if (m.chainId != MAINNET_CHAIN_ID) revert BadMainnetPin("chain id");
        if (m.originToken != MAINNET_USDT) revert BadMainnetPin("origin token");
        if (m.minBridgeValidators != DEFAULT_MIN_BRIDGE_VALIDATORS) revert BadMainnetPin("MIN_BRIDGE_VALIDATORS");
        if (m.maxBridgeValidators != DEFAULT_MAX_BRIDGE_VALIDATORS) revert BadMainnetPin("MAX_BRIDGE_VALIDATORS");
        if (m.minBridgeTotalWeight != DEFAULT_MIN_BRIDGE_TOTAL_WEIGHT) revert BadMainnetPin("MIN_BRIDGE_TOTAL_WEIGHT");
        if (m.maxDepositRaw != DEFAULT_MAX_DEPOSIT_RAW) revert BadMainnetPin("MAX_DEPOSIT_RAW");
        if (m.minEpochDuration != B10_MIN_EPOCH_DURATION) revert BadMainnetPin("MIN_EPOCH_DURATION");
        if (m.maxEpochLag != B10_MAX_EPOCH_LAG) revert BadMainnetPin("MAX_EPOCH_LAG");
        if (m.depositCloseAfter != B10_DEPOSIT_CLOSE_AFTER) revert BadMainnetPin("DEPOSIT_CLOSE_AFTER");
        if (m.maxCertificateAge != B10_MAX_CERTIFICATE_AGE) revert BadMainnetPin("MAX_CERTIFICATE_AGE");
        if (m.reviewWindow != MAINNET_REVIEW_WINDOW) revert BadMainnetPin("REVIEW_WINDOW");
        if (m.initDeadline <= m.nowTimestamp || m.initDeadline - m.nowTimestamp > MAINNET_MAX_INIT_WINDOW) {
            revert BadMainnetPin("INIT_DEADLINE");
        }
        if (
            m.sMax == 0 || m.sMax > m.depositCloseAfter
                || m.depositCloseAfter < CLOSE_EPOCH_BLOCKS * m.sMax + CLOSE_RELAY_MARGIN
        ) revert BadMainnetPin("S_MAX");
        validateWindows(
            m.chainId,
            Windows({
                minEpochDuration: m.minEpochDuration,
                maxEpochLag: m.maxEpochLag,
                depositCloseAfter: m.depositCloseAfter,
                maxCertificateAge: m.maxCertificateAge,
                initWindow: m.initDeadline - m.nowTimestamp,
                reviewWindow: m.reviewWindow
            })
        );
    }

    /// deployments/local-<name>.json with no path separator in <name>: inside
    /// the script's write permission (./deployments) and the .gitignore entry
    /// deployments/local-*.json.
    function validateManifestPath(string memory path) public pure {
        bytes memory b = bytes(path);
        bytes memory prefix = bytes(MANIFEST_PREFIX);
        bytes memory suffix = bytes(MANIFEST_SUFFIX);
        if (b.length <= prefix.length + suffix.length) revert BadManifestPath(path);
        for (uint256 i = 0; i < prefix.length; ++i) {
            if (b[i] != prefix[i]) revert BadManifestPath(path);
        }
        uint256 nameEnd = b.length - suffix.length;
        for (uint256 i = 0; i < suffix.length; ++i) {
            if (b[nameEnd + i] != suffix[i]) revert BadManifestPath(path);
        }
        for (uint256 i = prefix.length; i < nameEnd; ++i) {
            if (b[i] == "/" || b[i] == "\\") revert BadManifestPath(path);
        }
    }

    /// Reads every immutable back and compares it with `p` and `d`; also
    /// checks the four addresses are the deployer's CREATEs at nonces n..n+3,
    /// so a predicted forwarder factory (nonce n+4) is still reachable. With
    /// forwarders, the F_d factory is that n+4 CREATE and the F_x factory the
    /// n+5 one; without them, `d` names no forwarder factory.
    function verifyDeployment(Params memory p, Deployment memory d) public view {
        _check(address(d.prover).code.length != 0 && address(d.prover).codehash == d.proverCodeHash, "prover code");
        _check(
            address(d.verifier).code.length != 0 && address(d.verifier).codehash == d.verifierCodeHash, "verifier code"
        );
        _check(address(d.vault).code.length != 0 && address(d.vault).codehash == d.vaultCodeHash, "vault code");
        _check(address(d.batch).code.length != 0 && address(d.batch).codehash == d.batchCodeHash, "batch code");
        uint256 n = d.deployerNonce;
        _check(address(d.prover) == vm.computeCreateAddress(p.deployer, n), "prover address");
        _check(address(d.verifier) == vm.computeCreateAddress(p.deployer, n + 1), "verifier address");
        _check(address(d.vault) == vm.computeCreateAddress(p.deployer, n + 2), "vault address");
        _check(address(d.batch) == vm.computeCreateAddress(p.deployer, n + 3), "batch address");
        if (p.deployForwarders) {
            _check(
                d.forwarderFactoryPredicted
                    && d.forwarderFactory == vm.computeCreateAddress(p.deployer, n + FORWARDER_FACTORY_NONCE_OFFSET)
                    && address(d.depositFactory) == d.forwarderFactory,
                "deposit forwarder factory address"
            );
            _check(
                address(d.exitFactory) == vm.computeCreateAddress(p.deployer, n + EXIT_FORWARDER_FACTORY_NONCE_OFFSET),
                "exit forwarder factory address"
            );
        } else {
            _check(
                address(d.depositFactory) == address(0) && d.depositFactoryCodeHash == bytes32(0)
                    && address(d.exitFactory) == address(0) && d.exitFactoryCodeHash == bytes32(0)
                    && d.forwarderInitCodeHash == bytes32(0),
                "no forwarder factories"
            );
            if (d.forwarderFactoryPredicted) {
                _check(
                    d.forwarderFactory == vm.computeCreateAddress(p.deployer, n + FORWARDER_FACTORY_NONCE_OFFSET)
                        && d.forwarderFactory.code.length == 0,
                    "predicted forwarder factory"
                );
            } else {
                _check(d.forwarderFactory == p.forwarderFactory, "forwarder factory");
            }
        }
        _verifyVerifier(p, d);
        _verifyVault(p, d);
        if (p.deployForwarders) _verifyForwarders(p, d);
    }

    // ================================================================ manifest

    /// The manifest JSON (snake_case keys; integers as JSON numbers, hashes as 0x-hex).
    function manifestJson(Params memory p, Deployment memory d) public returns (string memory json) {
        string memory k = "b3_bridge_v2_deployment";
        vm.serializeString(k, "schema", MANIFEST_SCHEMA);
        vm.serializeString(k, "status", "regtest_candidate");
        vm.serializeBool(k, "production_approved", false);
        vm.serializeUint(k, "chain_id", block.chainid);
        vm.serializeAddress(k, "deployer", p.deployer);
        vm.serializeUint(k, "deployer_nonce", d.deployerNonce);

        vm.serializeAddress(k, "prover", address(d.prover));
        vm.serializeBytes32(k, "prover_code_hash", d.proverCodeHash);
        vm.serializeAddress(k, "verifier", address(d.verifier));
        vm.serializeBytes32(k, "verifier_code_hash", d.verifierCodeHash);
        vm.serializeAddress(k, "vault", address(d.vault));
        vm.serializeBytes32(k, "vault_code_hash", d.vaultCodeHash);
        vm.serializeAddress(k, "relay_batch", address(d.batch));
        vm.serializeBytes32(k, "relay_batch_code_hash", d.batchCodeHash);
        vm.serializeBytes32(k, "b3_asset_id", d.assetId);

        vm.serializeBytes32(k, "chain_domain", p.chainDomain);
        vm.serializeUint(k, "genesis_height", p.genesisHeight);
        vm.serializeBytes32(k, "genesis_block_hash", p.genesisBlockHash);
        vm.serializeBytes32(k, "genesis_set_hash", p.genesisSetHash);
        vm.serializeBytes32(k, "lineage_id", p.lineageId);
        vm.serializeUint(k, "start_epoch", p.startEpoch);
        vm.serializeBytes32(k, "start_set_hash", p.startSetHash);
        vm.serializeUint(k, "bootstrap_mode", uint256(p.bootstrapMode));
        vm.serializeString(
            k,
            "bootstrap_mode_name",
            p.bootstrapMode == B3FinalityVerifierV2.BootstrapMode.K_OF_N ? "K_OF_N" : "START_CERTIFICATE"
        );
        vm.serializeBytes32(k, "bootstrap_set_hash", p.bootstrapSetHash);
        vm.serializeBytes(k, "bootstrap_aggregate_pubkey", p.bootstrapAggregatePubkey);
        vm.serializeBytes32(k, "bootstrap_members_root", p.bootstrapMembersRoot);
        vm.serializeUint(k, "bridge_activation_height", p.bridgeActivationHeight);
        vm.serializeUint(k, "min_bridge_validators", p.minBridgeValidators);
        vm.serializeUint(k, "max_bridge_validators", p.maxBridgeValidators);
        vm.serializeUint(k, "min_bridge_total_weight", p.minBridgeTotalWeight);

        vm.serializeUint(k, "min_epoch_duration", p.windows.minEpochDuration);
        vm.serializeUint(k, "max_epoch_lag", p.windows.maxEpochLag);
        vm.serializeUint(k, "deposit_close_after", p.windows.depositCloseAfter);
        vm.serializeUint(k, "max_certificate_age", p.windows.maxCertificateAge);
        vm.serializeUint(k, "init_window", p.windows.initWindow);
        vm.serializeUint(k, "init_deadline", d.initDeadline);
        vm.serializeUint(k, "review_window", p.windows.reviewWindow);
        vm.serializeAddress(k, "watchers", p.watchers);

        vm.serializeAddress(k, "origin_token", p.originToken);
        vm.serializeBytes32(k, "token_code_hash", d.tokenCodeHash);
        vm.serializeUint(k, "max_deposit_raw", p.maxDepositRaw);
        vm.serializeAddress(k, "predecessor", address(0));
        vm.serializeAddress(k, "forwarder_factory", d.forwarderFactory);
        uint256 factoryNonce = uint256(d.deployerNonce) + FORWARDER_FACTORY_NONCE_OFFSET;
        vm.serializeUint(k, "forwarder_factory_create_nonce", factoryNonce);
        vm.serializeBool(k, "forwarder_factory_predicted", d.forwarderFactoryPredicted);
        // A codeless factory: a later deployment at that address decides the
        // vault's caller check (see the contract comment).
        bool factoryCodeless = d.forwarderFactory.code.length == 0;
        vm.serializeBytes32(
            k, "forwarder_factory_code_hash", factoryCodeless ? bytes32(0) : d.forwarderFactory.codehash
        );
        vm.serializeBool(k, "forwarder_factory_latent_deployer_privilege", factoryCodeless);

        // Forwarder factories (routes (b), (d3)): every key is written, zero
        // when not deployed. B3's deposit_forwarder pin is forwarder_factory
        // with forwarder_init_code_hash; its exit_forwarder pin is
        // exit_forwarder_factory with the same hash and exit_dst_eid.
        bool fwd = p.deployForwarders;
        vm.serializeBool(k, "deploy_forwarders", fwd);
        vm.serializeAddress(k, "exit_forwarder_factory", address(d.exitFactory));
        vm.serializeBytes32(k, "exit_forwarder_factory_code_hash", d.exitFactoryCodeHash);
        vm.serializeBytes32(k, "forwarder_init_code_hash", d.forwarderInitCodeHash);
        vm.serializeAddress(k, "oft_adapter", fwd ? p.oftAdapter : address(0));
        vm.serializeUint(k, "exit_dst_eid", fwd ? p.exitDstEid : uint32(0));
        json = vm.serializeBytes(k, "exit_extra_options", fwd ? p.exitExtraOptions : bytes(""));
    }

    // =============================================================== internals

    function _verifierConfig(Params memory p, Deployment memory d)
        private
        pure
        returns (B3FinalityVerifierV2.Config memory c)
    {
        c.chainDomain = p.chainDomain;
        c.genesisHeight = p.genesisHeight;
        c.genesisBlockHash = p.genesisBlockHash;
        c.genesisSetHash = p.genesisSetHash;
        c.lineageId = p.lineageId;
        c.startEpoch = p.startEpoch;
        c.startSetHash = p.startSetHash;
        c.bootstrapMode = p.bootstrapMode;
        c.bootstrapSetHash = p.bootstrapSetHash;
        c.prover = d.prover;
        c.proverCodeHash = d.proverCodeHash;
        c.bridgeActivationHeight = p.bridgeActivationHeight;
        c.minBridgeValidators = p.minBridgeValidators;
        c.maxBridgeValidators = p.maxBridgeValidators;
        c.minBridgeTotalWeight = p.minBridgeTotalWeight;
        c.minEpochDuration = p.windows.minEpochDuration;
        c.maxEpochLag = p.windows.maxEpochLag;
        c.depositCloseAfter = p.windows.depositCloseAfter;
        c.maxCertificateAge = p.windows.maxCertificateAge;
        c.initDeadline = d.initDeadline;
        c.reviewWindow = p.windows.reviewWindow;
    }

    function _verifyVerifier(Params memory p, Deployment memory d) private view {
        B3FinalityVerifierV2 v = d.verifier;
        _check(v.CHAIN_DOMAIN() == p.chainDomain, "CHAIN_DOMAIN");
        _check(v.GENESIS_HEIGHT() == p.genesisHeight, "GENESIS_HEIGHT");
        _check(v.GENESIS_BLOCK_HASH() == p.genesisBlockHash, "GENESIS_BLOCK_HASH");
        _check(v.GENESIS_SET_HASH() == p.genesisSetHash, "GENESIS_SET_HASH");
        _check(
            v.LINEAGE_ID() == p.lineageId
                && v.LINEAGE_ID()
                    == B3TypesV2.lineageId(p.chainDomain, p.genesisHeight, p.genesisBlockHash, p.genesisSetHash),
            "LINEAGE_ID"
        );
        _check(v.START_EPOCH() == p.startEpoch, "START_EPOCH");
        _check(v.START_SET_HASH() == p.startSetHash, "START_SET_HASH");
        _check(v.BOOTSTRAP_MODE() == p.bootstrapMode, "BOOTSTRAP_MODE");
        _check(v.BOOTSTRAP_SET_HASH() == p.bootstrapSetHash, "BOOTSTRAP_SET_HASH");
        _check(address(v.PROVER()) == address(d.prover), "PROVER");
        _check(v.PROVER_CODE_HASH() == d.proverCodeHash, "PROVER_CODE_HASH");
        _check(v.BRIDGE_ACTIVATION_HEIGHT() == p.bridgeActivationHeight, "BRIDGE_ACTIVATION_HEIGHT");
        _check(v.MIN_BRIDGE_VALIDATORS() == p.minBridgeValidators, "MIN_BRIDGE_VALIDATORS");
        _check(v.MAX_BRIDGE_VALIDATORS() == p.maxBridgeValidators, "MAX_BRIDGE_VALIDATORS");
        _check(v.MIN_BRIDGE_TOTAL_WEIGHT() == p.minBridgeTotalWeight, "MIN_BRIDGE_TOTAL_WEIGHT");
        _check(v.MIN_EPOCH_DURATION() == p.windows.minEpochDuration, "MIN_EPOCH_DURATION");
        _check(v.MAX_EPOCH_LAG() == p.windows.maxEpochLag, "MAX_EPOCH_LAG");
        _check(v.DEPOSIT_CLOSE_AFTER() == p.windows.depositCloseAfter, "DEPOSIT_CLOSE_AFTER");
        _check(v.MAX_CERTIFICATE_AGE() == p.windows.maxCertificateAge, "MAX_CERTIFICATE_AGE");
        _check(v.INIT_DEADLINE() == d.initDeadline, "INIT_DEADLINE");
        _check(v.REVIEW_WINDOW() == p.windows.reviewWindow, "REVIEW_WINDOW");
        address[] memory watchers = v.watchers();
        _check(watchers.length == p.watchers.length, "WATCHERS");
        for (uint256 i = 0; i < watchers.length; ++i) {
            _check(watchers[i] == p.watchers[i], "WATCHERS");
        }
        _check(v.status() == B3TypesV2.STATUS_UNINITIALIZED, "verifier status");
    }

    function _verifyVault(Params memory p, Deployment memory d) private view {
        B3VaultV2 vault = d.vault;
        _check(address(vault.VERIFIER()) == address(d.verifier), "VERIFIER");
        _check(vault.VERIFIER_CODE_HASH() == d.verifierCodeHash, "VERIFIER_CODE_HASH");
        _check(vault.B3_CHAIN_DOMAIN() == p.chainDomain, "B3_CHAIN_DOMAIN");
        _check(vault.ORIGIN_TOKEN() == p.originToken, "ORIGIN_TOKEN");
        _check(vault.TOKEN_CODE_HASH() == d.tokenCodeHash, "TOKEN_CODE_HASH");
        _check(vault.FORWARDER_FACTORY() == d.forwarderFactory, "FORWARDER_FACTORY");
        _check(vault.MAX_DEPOSIT_RAW() == p.maxDepositRaw, "MAX_DEPOSIT_RAW");
        _check(vault.BRIDGE_ACTIVATION_HEIGHT() == p.bridgeActivationHeight, "vault BRIDGE_ACTIVATION_HEIGHT");
        _check(vault.ORIGIN_CHAIN_ID() == block.chainid, "ORIGIN_CHAIN_ID");
        _check(vault.B3_ASSET_ID() == d.assetId, "B3_ASSET_ID");
        _check(vault.PREDECESSOR() == address(0), "PREDECESSOR");
        _check(vault.nextDepositId() == 0 && vault.locked() == 0 && vault.absorbedTotal() == 0, "vault state");
    }

    function _verifyForwarders(Params memory p, Deployment memory d) private view {
        bytes32 initCodeHash = keccak256(type(B3Forwarder).creationCode);
        _check(d.forwarderInitCodeHash == initCodeHash, "forwarder init code hash");

        B3DepositForwarderFactory fd = d.depositFactory;
        _check(
            address(fd).code.length != 0 && address(fd).codehash == d.depositFactoryCodeHash,
            "deposit forwarder factory code"
        );
        _check(fd.VAULT() == address(d.vault), "F_d VAULT");
        _check(fd.TOKEN() == p.originToken, "F_d TOKEN");
        _check(fd.MAX_DEPOSIT_RAW() == p.maxDepositRaw, "F_d MAX_DEPOSIT_RAW");
        _check(fd.MIN_FLUSH_RAW() == FORWARDER_MIN_FLUSH_RAW, "F_d MIN_FLUSH_RAW");
        _check(fd.FORWARDER_INIT_CODE_HASH() == initCodeHash, "F_d FORWARDER_INIT_CODE_HASH");

        B3OftExitFactory fx = d.exitFactory;
        _check(
            address(fx).code.length != 0 && address(fx).codehash == d.exitFactoryCodeHash, "exit forwarder factory code"
        );
        _check(fx.TOKEN() == p.originToken, "F_x TOKEN");
        _check(fx.ADAPTER() == p.oftAdapter, "F_x ADAPTER");
        _check(fx.DST_EID() == p.exitDstEid, "F_x DST_EID");
        _check(keccak256(fx.extraOptions()) == keccak256(p.exitExtraOptions), "F_x extraOptions");
        _check(fx.FORWARDER_INIT_CODE_HASH() == initCodeHash, "F_x FORWARDER_INIT_CODE_HASH");
    }

    function _check(bool ok, string memory what) private pure {
        if (!ok) revert PostCheckFailed(what);
    }

    function _requireRegtest() private view {
        if (block.chainid != REGTEST_CHAIN_ID) revert NotRegtest(block.chainid);
    }

    function _envUint64(string memory name) private view returns (uint64) {
        uint256 value = vm.envUint(name);
        if (value > type(uint64).max) revert IntegerOutOfRange(name);
        // forge-lint: disable-next-line(unsafe-typecast)
        return uint64(value);
    }

    function _envOrUint64(string memory name, uint64 defaultValue) private view returns (uint64) {
        uint256 value = vm.envOr(name, uint256(defaultValue));
        if (value > type(uint64).max) revert IntegerOutOfRange(name);
        // forge-lint: disable-next-line(unsafe-typecast)
        return uint64(value);
    }

    function _envOrUint32(string memory name, uint32 defaultValue) private view returns (uint32) {
        uint256 value = vm.envOr(name, uint256(defaultValue));
        if (value > type(uint32).max) revert IntegerOutOfRange(name);
        // forge-lint: disable-next-line(unsafe-typecast)
        return uint32(value);
    }
}
