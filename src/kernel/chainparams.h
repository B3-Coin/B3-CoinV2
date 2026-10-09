// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_KERNEL_CHAINPARAMS_H
#define BITCOIN_KERNEL_CHAINPARAMS_H

#include <consensus/params.h>
#include <kernel/messagestartchars.h>
#include <primitives/block.h>
#include <uint256.h>
#include <util/chaintype.h>
#include <util/hash_type.h>
#include <util/vector.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct AssumeutxoHash : public BaseHash<uint256> {
    explicit AssumeutxoHash(const uint256& hash) : BaseHash(hash) {}
};

/**
 * Holds configuration for use during UTXO snapshot load and validation. The contents
 * here are security critical, since they dictate which UTXO snapshots are recognized
 * as valid.
 */
struct AssumeutxoData {
    int height;

    //! The expected hash of the deserialized UTXO set.
    AssumeutxoHash hash_serialized;

    //! Used to populate the m_chain_tx_count value, which is used during BlockManager::LoadBlockIndex().
    //!
    //! We need to hardcode the value here because this is computed cumulatively using block data,
    //! which we do not necessarily have at the time of snapshot load.
    uint64_t m_chain_tx_count;

    //! The hash of the base block for this snapshot. Used to refer to assumeutxo data
    //! prior to having a loaded blockindex.
    uint256 blockhash;
};

/**
 * Holds various statistics on transactions within a chain. Used to estimate
 * verification progress during chain sync.
 *
 * See also: CChainParams::TxData, GuessVerificationProgress.
 */
struct ChainTxData {
    int64_t nTime;    //!< UNIX timestamp of last known number of transactions
    uint64_t tx_count; //!< total number of transactions between genesis and that timestamp
    double dTxRate;   //!< estimated number of transactions per second after that timestamp
};

//! Configuration for headers sync memory usage.
struct HeadersSyncParams {
    //! Distance in blocks between header commitments.
    size_t commitment_period{0};
    //! Minimum number of validated headers to accumulate in the redownload
    //! buffer before feeding them into the permanent block index.
    size_t redownload_buffer_size{0};
};

/**
 * CChainParams defines various tweakable parameters of a given instance of the
 * Bitcoin system.
 */
class CChainParams
{
public:
    enum Base58Type {
        PUBKEY_ADDRESS,
        SCRIPT_ADDRESS,
        SECRET_KEY,
        EXT_PUBLIC_KEY,
        EXT_SECRET_KEY,

        MAX_BASE58_TYPES
    };

    const Consensus::Params& GetConsensus() const { return consensus; }
    const MessageStartChars& MessageStart() const { return pchMessageStart; }
    uint16_t GetDefaultPort() const { return nDefaultPort; }
    std::vector<int> GetAvailableSnapshotHeights() const;

    const CBlock& GenesisBlock() const { return genesis; }
    /** Default value for -checkmempool and -checkblockindex argument */
    bool DefaultConsistencyChecks() const { return fDefaultConsistencyChecks; }
    /** If this chain is exclusively used for testing */
    bool IsTestChain() const { return m_chain_type != ChainType::MAIN; }
    /** If this chain allows time to be mocked */
    bool IsMockableChain() const { return m_is_mockable_chain; }
    uint64_t PruneAfterHeight() const { return nPruneAfterHeight; }
    /** Minimum free space (in GB) needed for data directory */
    uint64_t AssumedBlockchainSize() const { return m_assumed_blockchain_size; }
    /** Minimum free space (in GB) needed for data directory when pruned; Does not include prune target*/
    uint64_t AssumedChainStateSize() const { return m_assumed_chain_state_size; }
    /** Whether it is possible to mine blocks on demand (no retargeting) */
    bool MineBlocksOnDemand() const { return consensus.fPowNoRetargeting; }
    /** Return the chain type string */
    std::string GetChainTypeString() const { return ChainTypeToString(m_chain_type); }
    /** Return the chain type */
    ChainType GetChainType() const { return m_chain_type; }
    /** Return the list of hostnames to look up for DNS seeds */
    const std::vector<std::string>& DNSSeeds() const { return vSeeds; }
    const std::vector<unsigned char>& Base58Prefix(Base58Type type) const { return base58Prefixes[type]; }
    const std::string& Bech32HRP() const { return bech32_hrp; }
    const std::vector<uint8_t>& FixedSeeds() const { return vFixedSeeds; }
    /** Modern-capable discovery hints used by B3's bounded transition rescue. */
    const std::vector<uint8_t>& ModernRecoverySeeds() const { return vModernRecoverySeeds; }
    const HeadersSyncParams& HeadersSync() const { return m_headers_sync_params; }

    std::optional<AssumeutxoData> AssumeutxoForHeight(int height) const
    {
        return FindFirst(m_assumeutxo_data, [&](const auto& d) { return d.height == height; });
    }
    std::optional<AssumeutxoData> AssumeutxoForBlockhash(const uint256& blockhash) const
    {
        return FindFirst(m_assumeutxo_data, [&](const auto& d) { return d.blockhash == blockhash; });
    }

    const ChainTxData& TxData() const { return chainTxData; }

    /**
     * SHA256 of the raw -b3bridgepins file bytes this regtest chain applied,
     * or nullopt when no pin file is in use (always nullopt outside the
     * -b3modernregtest -b3flowmeshtest regtest configuration).
     */
    const std::optional<uint256>& B3BridgePinsHash() const { return m_b3_bridge_pins_sha256; }

    /**
     * SigNetOptions holds configurations for creating a signet CChainParams.
     */
    struct SigNetOptions {
        std::optional<std::vector<uint8_t>> challenge{};
        std::optional<std::vector<std::string>> seeds{};
    };

    /**
     * VersionBitsParameters holds activation parameters
     */
    struct VersionBitsParameters {
        int64_t start_time;
        int64_t timeout;
        int min_activation_height;
    };

    /**
     * External V1 decentralized-bridge pins read from a -b3bridgepins file
     * (REGTEST ONLY, and only with -b3modernregtest -b3flowmeshtest). They
     * point the synthetic flowmesh-test bUSD at a local anvil deployment and
     * a synthetic beacon checkpoint. The origin chain stays 31337 and the
     * activation heights stay at A3, so every pin takes effect from the
     * bridge activation height.
     *
     * A file may instead (or also) carry a bridge V2 section (`v2`): one
     * fresh anvil Vault V2 entry, its fresh light-client store and the
     * Verifier V2 bounds, applied from V >= A3 (consensus.bridge_v2). With
     * no V1 section the synthetic MANAGED_V1 bUSD stays as without a file.
     */
    struct B3RegTestBridgePins {
        //! Synthetic flowmesh-test mint caps (raw bUSD units), used whenever
        //! a pin file does not override them.
        static constexpr CAmount DEFAULT_MAX_PER_BLOCK{1'000'000'000};
        static constexpr CAmount DEFAULT_MAX_PER_EPOCH{10'000'000'000};
        //! B-14 bridge V2 caps (10,000 bUSD per block and per mint epoch),
        //! used whenever the v2 section does not override them.
        static constexpr CAmount DEFAULT_V2_MAX_PER_BLOCK{10'000'000'000};
        static constexpr CAmount DEFAULT_V2_MAX_PER_EPOCH{10'000'000'000};

        /**
         * The v2 section (node changes sections 3.1-3.3 and 12.2). OI-3:
         * every V2 pin must be in the file before the chain first stores a
         * block at or above A3 (the -b3bridgepins record then refuses any
         * change, and V >= A3), so only values knowable before then are
         * keys. The vault and verifier runtime code hashes and the lineage
         * id commit to blockHash(R_f - 1) and are deliberately absent:
         * BridgeV2Params leaves both code hashes unset, which
         * BridgeV2Params::Valid() accepts only off Ethereum mainnet. A
         * window's value is a deployment parameter fixed in advance, so the
         * verifier's MAX_EPOCH_LAG may be named (verifier_max_epoch_lag).
         */
        struct V2Section {
            //! Always the local anvil chain (31337).
            uint64_t origin_chain_id{0};
            Consensus::BridgeEthAddress vault{};
            Consensus::BridgeEthAddress token{};
            Consensus::BridgeEthAddress verifier{};
            //! Lower bound of the first anvil block that may hold vault
            //! events (Vault V2 is deployed after R_f - 1).
            uint64_t origin_deployment_block{0};
            uint256 token_runtime_code_hash{};
            CAmount max_deposit_raw{0};
            std::optional<CAmount> max_per_block{};
            std::optional<CAmount> max_per_epoch{};
            Consensus::EthereumLightClientPins light_client{};
            uint32_t min_bridge_validators{0};
            uint32_t max_bridge_validators{0};
            uint64_t min_bridge_total_weight{0};
            //! V: first height of V2 records and of the V2 withdrawal tree.
            int activation_height{0};
            //! W2 >= V; V when unset (B-12).
            std::optional<int> burn_height{};
            //! The Verifier V2 MAX_EPOCH_LAG DeployV2 will use; when set it
            //! must equal the regtest burn gate's set expiry (profile R).
            std::optional<int64_t> verifier_max_epoch_lag{};
            //! Owner ruling R10: the first height (>= V) of the sponsored
            //! sighash rule for kind 6 burns; the rule stays off when unset.
            std::optional<int> sponsored_sighash_height{};

            /**
             * The optional B-23 successor vault v' (`v2.successor`; node
             * changes section 7.6). Pinned, like v, before the chain first
             * stores a block at or above A3 (OI-3), and applied only from
             * migration_height h_m > V (h_m >= W2). Only v' itself is keyed:
             * its CREATE address (a dedicated deployer at nonce 0), v as its
             * predecessor, the bUSD V2 AssetId it carries (raw EVM bytes,
             * Vault V2 B3_ASSET_ID, which RegTest() requires to be v's) and
             * optionally a deployment-block lower bound (default v's). v's
             * chain, token, token code hash, decimals, proof kind,
             * MAX_DEPOSIT_RAW and caps are inherited; the vault code hash of
             * v' stays unset (OI-3), so naming it is an unknown key.
             */
            struct SuccessorSection {
                Consensus::BridgeEthAddress vault{};
                Consensus::BridgeEthAddress predecessor{};
                uint256 asset_id{};
                int migration_height{0};
                std::optional<uint64_t> origin_deployment_block{};
            };
            std::optional<SuccessorSection> successor{};

            /**
             * The optional route (b) and (d3) forwarder factories
             * (`v2.deposit_forwarder`, `v2.exit_forwarder`; node changes
             * section 3.1, contracts spec sections 7-8). Knowable before
             * the first start, so pinned with v (OI-3): each factory is the
             * DeployV2 deployer's CREATE address (nonces n+4 and n+5) and
             * the init-code hash is keccak256 of B3Forwarder's creation
             * code from the forge artifact (raw EVM bytes). The sections
             * are the consensus pins themselves.
             */
            using DepositForwarderSection = Consensus::BridgeV2DepositForwarderPins;
            using ExitForwarderSection = Consensus::BridgeV2ExitForwarderPins;
            std::optional<DepositForwarderSection> deposit_forwarder{};
            std::optional<ExitForwarderSection> exit_forwarder{};
        };

        //! Whether the file carried a v1_decentralized section. True by
        //! default so hand-built pins keep their V1 meaning.
        bool has_v1{true};
        Consensus::BridgeEthAddress vault{};
        Consensus::BridgeEthAddress token{};
        uint64_t origin_deployment_block{0};
        uint256 vault_runtime_code_hash{};
        //! Origin-token runtime code hash: the direct-token adapter v1
        //! meaning of BridgeAssetParams::implementation_or_adapter.
        uint256 token_runtime_code_hash{};
        std::optional<CAmount> max_per_block{};
        std::optional<CAmount> max_per_epoch{};
        Consensus::EthereumLightClientPins light_client{};
        Consensus::BridgeDecentralizedWithdrawalPins withdrawal{};
        //! The bridge V2 section, when the file carried one.
        std::optional<V2Section> v2{};
        //! SHA256 of the raw pin file bytes.
        uint256 file_sha256{};
    };

    /**
     * RegTestOptions holds configurations for creating a regtest CChainParams.
     */
    /**
     * B3 modern-era regtest overrides (architecture contract section 64: the
     * sanctioned regtest/testnet activation-override facility; REGTEST ONLY,
     * never a mainnet surface). H = 0: the untouched regtest genesis is the
     * entire legacy era and X is its hash, so the fail-closed X rule keeps
     * its shape and no legacy history is fabricated. The corridor runs from
     * height 1, modern PoS from corridor_length + 1, with scaled scaffolding
     * constants exactly as the unit fixtures scale them.
     */
    struct B3ModernRegTestOptions {
        /**
         * Enable the complete FN/assets/FlowMesh release-test schedule.
         * This is deliberately nested under the B3 modern regtest options so
         * no synthetic manifest, treasury destination, or activation height
         * can leak into a real network configuration.
         */
        bool flowmesh_test{false};
        int corridor_length{160};
        //! Corridor pacing (seconds); scaled from the ratified 60 so a
        //! functional test can mine the corridor in real time.
        int64_t corridor_spacing{1};
        //! Corridor coinbase reward (base units) so a fresh chain can fund
        //! stakes (the ratified corridor is fees-only; scaffolding only).
        int64_t corridor_reward{2'000'000'000};
        //! Modern-PoS pacing (seconds); scaled from the ratified 60/30.
        int64_t block_interval{1};
        int64_t round_seconds{1};
        int epoch_length{30};
        int checkpoint_interval{5};
        int checkpoint_depth{3};
        int max_epoch_extension{30};
        int min_finality_set{1};
        int reorg_horizon{200};
        int64_t min_stake_amount{1000};
        //! OD-2 schedule scaffolding: modern per-block subsidy R0, halving
        //! interval (blocks), treasury percent and treasury script (hex).
        int64_t modern_reward{0};
        int64_t halving_interval{0};
        int64_t treasury_percent{0};
        std::vector<unsigned char> treasury_script{};
        //! Parsed -b3bridgepins file; applied only together with
        //! flowmesh_test.
        std::optional<B3RegTestBridgePins> bridge_pins{};
        //! -b3freshlineageheight (R_f) and -b3freshmaxepochextension: the
        //! fresh finality lineage with the B-7 staging signer (bridge V2
        //! regtest plan section 3.3). Applied only together with
        //! flowmesh_test; R_f - 1 must lie on the V1 checkpoint grid.
        std::optional<int> fresh_lineage_height{};
        std::optional<int> fresh_max_epoch_extension{};
        //! -b3freshkeyheight (R_key): B-37 per-epoch committee keys for the
        //! fresh lineage from this height (finality dependency section
        //! 6.4). Requires fresh_lineage_height; 1 <= R_key <= R_f - 2.
        //! Unset keeps the staging lineage on the V1 keys.
        std::optional<int> fresh_key_height{};
        //! B-23's fresh-lineage extension on regtest profile R.
        static constexpr int DEFAULT_FRESH_MAX_EPOCH_EXTENSION{1'280};
        //! -b3h1height (R_h) and -b3h1escaperounds (R_escape): H1 producer
        //! rotation (finality dependency section 5.1, FD-2). Applied only
        //! together with flowmesh_test; R_h > M. The escape rounds need R_h.
        std::optional<int> h1_height{};
        std::optional<int64_t> h1_escape_rounds{};
        //! The largest R_escape the block assembler's round search reaches
        //! (it tries rounds below its 100,000-round bound).
        static constexpr int64_t MAX_H1_ESCAPE_ROUNDS{99'999};
        //! -b3h2checkpoint=HEIGHT:HASH (H2, finality dependency section
        //! 5.3): extra hardened modern checkpoints, in modern_checkpoints.
        std::vector<std::pair<int, uint256>> h2_checkpoints{};
    };

    struct RegTestOptions {
        std::unordered_map<Consensus::DeploymentPos, VersionBitsParameters> version_bits_parameters{};
        std::unordered_map<Consensus::BuriedDeployment, int> activation_heights{};
        bool fastprune{false};
        bool enforce_bip94{false};
        std::optional<B3ModernRegTestOptions> b3_modern{};
    };

    static std::unique_ptr<const CChainParams> RegTest(const RegTestOptions& options);
    static std::unique_ptr<const CChainParams> SigNet(const SigNetOptions& options);
    static std::unique_ptr<const CChainParams> Main();
    static std::unique_ptr<const CChainParams> TestNet();
    static std::unique_ptr<const CChainParams> TestNet4();

protected:
    CChainParams() = default;

    Consensus::Params consensus;
    MessageStartChars pchMessageStart;
    uint16_t nDefaultPort;
    uint64_t nPruneAfterHeight;
    uint64_t m_assumed_blockchain_size;
    uint64_t m_assumed_chain_state_size;
    std::vector<std::string> vSeeds;
    std::vector<unsigned char> base58Prefixes[MAX_BASE58_TYPES];
    std::string bech32_hrp;
    ChainType m_chain_type;
    CBlock genesis;
    std::vector<uint8_t> vFixedSeeds;
    std::vector<uint8_t> vModernRecoverySeeds;
    bool fDefaultConsistencyChecks;
    bool m_is_mockable_chain;
    std::vector<AssumeutxoData> m_assumeutxo_data;
    ChainTxData chainTxData;
    HeadersSyncParams m_headers_sync_params;
    std::optional<uint256> m_b3_bridge_pins_sha256{};
};

std::optional<ChainType> GetNetworkForMagic(const MessageStartChars& pchMessageStart);

#endif // BITCOIN_KERNEL_CHAINPARAMS_H
