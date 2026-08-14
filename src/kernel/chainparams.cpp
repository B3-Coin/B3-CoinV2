// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <kernel/chainparams.h>

#include <chainparamsseeds.h>
#include <consensus/amount.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <crypto/hex_base.h>
#include <hash.h>
#include <kernel/messagestartchars.h>
#include <legacy/consensus.h>
#include <legacy/primitives.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <uint256.h>
#include <util/chaintype.h>
#include <util/log.h>
#include <util/strencodings.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <span>
#include <utility>

using namespace util::hex_literals;

namespace {

// BIP155-serialized CService entries for B3Coin IPv4 bootstrap peers. Each
// entry is:
// IPv4 network id, address length, IPv4 octets, then port 5647 (big-endian).
constexpr std::array<uint8_t, 32 * 8> B3COIN_FIXED_SEEDS{
    0x01, 0x04, 0x65, 0x6f, 0x59, 0x55, 0x16, 0x0f, // 101.111.89.85
    0x01, 0x04, 0xbc, 0x44, 0x34, 0xac, 0x16, 0x0f, // 188.68.52.172
    0x01, 0x04, 0x65, 0xb0, 0x76, 0x67, 0x16, 0x0f, // 101.176.118.103
    0x01, 0x04, 0x67, 0xff, 0x04, 0x36, 0x16, 0x0f, // 103.255.4.54
    0x01, 0x04, 0x68, 0xbd, 0x9f, 0xe4, 0x16, 0x0f, // 104.189.159.228
    0x01, 0x04, 0x6b, 0x96, 0x07, 0x7a, 0x16, 0x0f, // 107.150.7.122
    0x01, 0x04, 0x6b, 0xae, 0x80, 0xb3, 0x16, 0x0f, // 107.174.128.179
    0x01, 0x04, 0x6b, 0xbf, 0x68, 0x50, 0x16, 0x0f, // 107.191.104.80
    0x01, 0x04, 0x6c, 0xc4, 0xb9, 0xa4, 0x16, 0x0f, // 108.196.185.164
    0x01, 0x04, 0x6f, 0xdc, 0x8c, 0x53, 0x16, 0x0f, // 111.220.140.83
    0x01, 0x04, 0x70, 0x4e, 0x08, 0x87, 0x16, 0x0f, // 112.78.8.135
    0x01, 0x04, 0x72, 0x6d, 0xcc, 0x38, 0x16, 0x0f, // 114.109.204.56
    0x01, 0x04, 0x73, 0x91, 0x91, 0x20, 0x16, 0x0f, // 115.145.145.32
    0x01, 0x04, 0x73, 0xb2, 0xff, 0x67, 0x16, 0x0f, // 115.178.255.103
    0x01, 0x04, 0x73, 0x46, 0x95, 0x62, 0x16, 0x0f, // 115.70.149.98
    0x01, 0x04, 0x74, 0x0f, 0x6b, 0xf8, 0x16, 0x0f, // 116.15.107.248
    0x01, 0x04, 0x74, 0xce, 0xdf, 0x6f, 0x16, 0x0f, // 116.206.223.111
    0x01, 0x04, 0x7a, 0xa1, 0xae, 0x26, 0x16, 0x0f, // 122.161.174.38
    0x01, 0x04, 0x0d, 0x52, 0x5d, 0xbd, 0x16, 0x0f, // 13.82.93.189
    0x01, 0x04, 0x0d, 0x5e, 0x9a, 0x67, 0x16, 0x0f, // 13.94.154.103
    0x01, 0x04, 0x86, 0x77, 0xb5, 0x9d, 0x16, 0x0f, // 134.119.181.157
    // Operator-supplied live bootstrap peers.
    0x01, 0x04, 0xae, 0xc5, 0x01, 0xc4, 0x16, 0x0f, // 174.197.1.196
    0x01, 0x04, 0xae, 0xc5, 0x02, 0x1a, 0x16, 0x0f, // 174.197.2.26
    0x01, 0x04, 0xae, 0xf6, 0xc0, 0x25, 0x16, 0x0f, // 174.246.192.37
    0x01, 0x04, 0x2d, 0x4c, 0x24, 0xf9, 0x16, 0x0f, // 45.76.36.249
    0x01, 0x04, 0x46, 0x78, 0xb0, 0x3e, 0x16, 0x0f, // 70.120.176.62
    0x01, 0x04, 0x4d, 0xa9, 0xf6, 0xdc, 0x16, 0x0f, // 77.169.246.220
    0x01, 0x04, 0x51, 0x38, 0x1a, 0x29, 0x16, 0x0f, // 81.56.26.41
    0x01, 0x04, 0x51, 0x38, 0x2d, 0x62, 0x16, 0x0f, // 81.56.45.98
    0x01, 0x04, 0x5f, 0xb3, 0x97, 0xba, 0x16, 0x0f, // 95.179.151.186
    0x01, 0x04, 0x5f, 0xb3, 0x9d, 0x37, 0x16, 0x0f, // 95.179.157.55
    0x01, 0x04, 0x62, 0x61, 0x8f, 0x0e, 0x16, 0x0f, // 98.97.143.14
};

} // namespace

/**
 * The test chains keep Bitcoin Core's original genesis blocks. Their coinbase
 * value is the stock 50 BTC spelled in stock 1e8 subunits, independent of the
 * B3 COIN unit, so the historical genesis hashes remain byte-exact.
 */
static constexpr CAmount BITCOIN_GENESIS_REWARD{5'000'000'000};

static CBlock CreateGenesisBlock(const char* pszTimestamp, const CScript& genesisOutputScript, uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    CMutableTransaction txNew;
    txNew.version = 1;
    txNew.vin.resize(1);
    txNew.vout.resize(1);
    txNew.vin[0].scriptSig = CScript() << 486604799 << CScriptNum(4) << std::vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
    txNew.vout[0].nValue = genesisReward;
    txNew.vout[0].scriptPubKey = genesisOutputScript;

    CBlock genesis;
    genesis.nTime    = nTime;
    genesis.nBits    = nBits;
    genesis.nNonce   = nNonce;
    genesis.nVersion = nVersion;
    genesis.vtx.push_back(MakeTransactionRef(std::move(txNew)));
    genesis.hashPrevBlock.SetNull();
    genesis.hashMerkleRoot = BlockMerkleRoot(genesis);
    return genesis;
}

/**
 * Build the genesis block. Note that the output of its generation
 * transaction cannot be spent since it did not originally exist in the
 * database.
 *
 * CBlock(hash=000000000019d6, ver=1, hashPrevBlock=00000000000000, hashMerkleRoot=4a5e1e, nTime=1231006505, nBits=1d00ffff, nNonce=2083236893, vtx=1)
 *   CTransaction(hash=4a5e1e, ver=1, vin.size=1, vout.size=1, nLockTime=0)
 *     CTxIn(COutPoint(000000, -1), coinbase 04ffff001d0104455468652054696d65732030332f4a616e2f32303039204368616e63656c6c6f72206f6e206272696e6b206f66207365636f6e64206261696c6f757420666f722062616e6b73)
 *     CTxOut(nValue=50.00000000, scriptPubKey=0x5F1DF16B2B704C8A578D0B)
 *   vMerkleTree: 4a5e1e
 */
static CBlock CreateGenesisBlock(uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    const char* pszTimestamp = "The Times 03/Jan/2009 Chancellor on brink of second bailout for banks";
    const CScript genesisOutputScript = CScript() << "04678afdb0fe5548271967f1a67130b7105cd6a828e03909a67962e0ea1f61deb649f6bc3f4cef38c4f35504e51ec112de5c384df7ba0b8d578a4c702b6bf11d5f"_hex << OP_CHECKSIG;
    return CreateGenesisBlock(pszTimestamp, genesisOutputScript, nTime, nNonce, nBits, nVersion, genesisReward);
}

/**
 * Main network on which people trade goods and services.
 */
class CMainParams : public CChainParams {
public:
    CMainParams() {
        m_chain_type = ChainType::MAIN;
        consensus.legacy_b3coin = true;
        consensus.legacy_last_pow_block = 500;
        // Historical live-legacy checkpoint rules, ported verbatim.
        consensus.legacy_checkpoints = legacy::MainnetCheckpoints();
        consensus.legacy_checkpoint_span = legacy::LEGACY_CHECKPOINT_SPAN;
        // The historical one-off superblock (chainparams nSuperBlockHeight /
        // vSuperBlockPubKey in the final client, hex verbatim).
        consensus.legacy_superblock_height = 107'488;
        consensus.legacy_superblock_pubkey = ParseHex("0432160bdb95ec14c30a3c76ed742403a34d3b57841f49caec6971eee735bcc68d35d35936c66719910b32c51db72621191437d23659785fe20ee7268e7d340522");
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.nSubsidyHalvingInterval = 210000;
        consensus.BIP34Height = std::numeric_limits<int>::max();
        consensus.BIP34Hash = uint256{};
        consensus.BIP65Height = std::numeric_limits<int>::max();
        consensus.BIP66Height = std::numeric_limits<int>::max();
        consensus.CSVHeight = std::numeric_limits<int>::max();
        consensus.SegwitHeight = std::numeric_limits<int>::max();
        consensus.MinBIP9WarningHeight = std::numeric_limits<int>::max();
        consensus.powLimit = uint256{"00000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
        consensus.nPowTargetTimespan = 360 * 20;
        consensus.nPowTargetSpacing = 360;
        consensus.fPowAllowMinDifficultyBlocks = false;
        consensus.enforce_BIP94 = false;
        consensus.fPowNoRetargeting = false;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].threshold = 1815; // 90%
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].period = 2016;

        // Bitcoin deployments stay disabled until B3Coin's post-fork rules
        // are explicitly specified and activated.
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].bit = 2;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].min_activation_height = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].threshold = 1815; // 90%
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].period = 2016;

        consensus.nMinimumChainWork = uint256{};
        consensus.defaultAssumeValid = uint256{};

        /**
         * The message start string is designed to be unlikely to occur in normal data.
         * The characters are rarely used upper ASCII, not valid as UTF-8, and produce
         * a large 32-bit integer with any alignment.
         */
        // Preserve the final legacy B3Coin wire protocol so existing peers
        // can exchange the historical chain with B3Coin Core.
        pchMessageStart[0] = 0xb3;
        pchMessageStart[1] = 0x2e;
        pchMessageStart[2] = 0x1e;
        pchMessageStart[3] = 0xe6;
        nDefaultPort = 5647;
        nPruneAfterHeight = 1000;
        m_assumed_blockchain_size = 0;
        m_assumed_chain_state_size = 0;

        genesis = legacy::CreateCoreGenesisBlock();
        consensus.hashGenesisBlock = genesis.GetHash(consensus, /*height=*/0);
        assert(consensus.hashGenesisBlock == uint256{"4b0d7f133c5267d715d4d8992635a5490d1edd6b7072cce3f8fe116aba983b6a"});
        assert(genesis.hashMerkleRoot == uint256{"4243fd570d4cb2e2930767f5bf18b2f65f1b7c4e16a392552d1efadeec00753d"});

        // Core treats vSeeds as DNS hostnames. These legacy values are literal
        // IPv4 endpoints, so feed them through its fixed-seed path instead.
        // They are historical bootstrap addresses, not a claim that each is
        // still online.
        vSeeds.clear();
        vFixedSeeds.assign(B3COIN_FIXED_SEEDS.begin(), B3COIN_FIXED_SEEDS.end());

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,63);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,85);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,153);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x88, 0xB2, 0x1E};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x88, 0xAD, 0xE4};

        bech32_hrp = "b3";

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;

        m_assumeutxo_data = {
            {
                .height = 840'000,
                .hash_serialized = AssumeutxoHash{uint256{"a2a5521b1b5ab65f67818e5e8eccabb7171a517f9e2382208f77687310768f96"}},
                .m_chain_tx_count = 991032194,
                .blockhash = uint256{"0000000000000000000320283a032748cef8227873ff4872689bf23f1cda83a5"},
            },
            {
                .height = 880'000,
                .hash_serialized = AssumeutxoHash{uint256{"dbd190983eaf433ef7c15f78a278ae42c00ef52e0fd2a54953782175fbadcea9"}},
                .m_chain_tx_count = 1145604538,
                .blockhash = uint256{"000000000000000000010b17283c3c400507969a9c2afd1dcf2082ec5cca2880"},
            },
            {
                .height = 910'000,
                .hash_serialized = AssumeutxoHash{uint256{"4daf8a17b4902498c5787966a2b51c613acdab5df5db73f196fa59a4da2f1568"}},
                .m_chain_tx_count = 1226586151,
                .blockhash = uint256{"0000000000000000000108970acb9522ffd516eae17acddcb1bd16469194a821"},
            },
            {
                .height = 935'000,
                .hash_serialized = AssumeutxoHash{uint256{"e4b90ef9eae834f56c4b64d2d50143cee10ad87994c614d7d04125e2a6025050"}},
                .m_chain_tx_count = 1305397408,
                .blockhash = uint256{"0000000000000000000147034958af1652b2b91bba607beacc5e72a56f0fb5ee"},
            }
        };

        chainTxData = ChainTxData{
            // Data from RPC: getchaintxstats 4096 00000000000000000000ccebd6d74d9194d8dcdc1d177c478e094bfad51ba5ac
            .nTime    = 1772055173,
            .tx_count = 1315805869,
            .dTxRate  = 5.40111006496122,
        };

        // Generated by headerssync-params.py on 2026-02-25.
        m_headers_sync_params = HeadersSyncParams{
            .commitment_period = 641,
            .redownload_buffer_size = 15218, // 15218/641 = ~23.7 commitments
        };

        // No B3Coin AssumeUTXO snapshot has been independently verified yet.
        m_assumeutxo_data.clear();
        chainTxData = ChainTxData{.nTime = 0, .tx_count = 0, .dTxRate = 0.0};
        m_headers_sync_params = HeadersSyncParams{};
    }
};

/**
 * Testnet (v3): public test network which is reset from time to time.
 */
class CTestNetParams : public CChainParams {
public:
    CTestNetParams() {
        m_chain_type = ChainType::TESTNET;
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.nSubsidyHalvingInterval = 210000;
        consensus.script_flag_exceptions.emplace( // BIP16 exception
            uint256{"00000000dd30457c001f4095d208cc1296b0eed002427aa599874af7a432b105"}, SCRIPT_VERIFY_NONE);
        consensus.BIP34Height = 21111;
        consensus.BIP34Hash = uint256{"0000000023b3a96d3484e5abb3755c413e7d41500f8e2a5c3f0dd01299cd8ef8"};
        consensus.BIP65Height = 581885; // 00000000007f6655f22f98e72ed80d8b06dc761d5da09df0fa1dc4be4f861eb6
        consensus.BIP66Height = 330776; // 000000002104c8c45e99a8853285a3b592602a3ccde2b832481da85e9e4ba182
        consensus.CSVHeight = 770112; // 00000000025e930139bac5c6c31a403776da130831ab85be56578f3fa75369bb
        consensus.SegwitHeight = 834624; // 00000000002b980fcd729daaa248fd9316a5200e9b367f4ff2c42453e84201ca
        consensus.MinBIP9WarningHeight = 836640; // segwit activation height + miner confirmation window
        consensus.powLimit = uint256{"00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
        consensus.nPowTargetTimespan = 14 * 24 * 60 * 60; // two weeks
        consensus.nPowTargetSpacing = 10 * 60;
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.enforce_BIP94 = false;
        consensus.fPowNoRetargeting = false;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].threshold = 1512; // 75%
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].period = 2016;

        // Deployment of Taproot (BIPs 340-342)
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].bit = 2;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nStartTime = 1619222400; // April 24th, 2021
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nTimeout = 1628640000; // August 11th, 2021
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].min_activation_height = 0; // No activation delay
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].threshold = 1512; // 75%
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].period = 2016;

        consensus.nMinimumChainWork = uint256{"0000000000000000000000000000000000000000000017dde1c649f3708d14b6"};
        consensus.defaultAssumeValid = uint256{"000000007a61e4230b28ac5cb6b5e5a0130de37ac1faf2f8987d2fa6505b67f4"}; // 4842348

        pchMessageStart[0] = 0x0b;
        pchMessageStart[1] = 0x11;
        pchMessageStart[2] = 0x09;
        pchMessageStart[3] = 0x07;
        nDefaultPort = 18333;
        nPruneAfterHeight = 1000;
        m_assumed_blockchain_size = 245;
        m_assumed_chain_state_size = 19;

        genesis = CreateGenesisBlock(1296688602, 414098458, 0x1d00ffff, 1, BITCOIN_GENESIS_REWARD);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256{"000000000933ea01ad0ee984209779baaec3ced90fa3f408719526f8d77f4943"});
        assert(genesis.hashMerkleRoot == uint256{"4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b"});

        vFixedSeeds.clear();
        vSeeds.clear();
        // nodes with support for servicebits filtering should be at the top
        vSeeds.emplace_back("testnet-seed.bitcoin.jonasschnelli.ch.");
        vSeeds.emplace_back("seed.tbtc.petertodd.net.");
        vSeeds.emplace_back("seed.testnet.bitcoin.sprovoost.nl.");
        vSeeds.emplace_back("testnet-seed.bluematt.me."); // Just a static list of stable node(s), only supports x9
        vSeeds.emplace_back("seed.testnet.achownodes.xyz."); // Ava Chow, only supports x1, x5, x9, x49, x809, x849, xd, x400, x404, x408, x448, xc08, xc48, x40c

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,111);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,196);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        bech32_hrp = "tb";

        vFixedSeeds = std::vector<uint8_t>(std::begin(chainparams_seed_test), std::end(chainparams_seed_test));

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;

        m_assumeutxo_data = {
            {
                .height = 2'500'000,
                .hash_serialized = AssumeutxoHash{uint256{"f841584909f68e47897952345234e37fcd9128cd818f41ee6c3ca68db8071be7"}},
                .m_chain_tx_count = 66484552,
                .blockhash = uint256{"0000000000000093bcb68c03a9a168ae252572d348a2eaeba2cdf9231d73206f"},
            },
            {
                .height = 4'840'000,
                .hash_serialized = AssumeutxoHash{uint256{"ce6bb677bb2ee9789c4a1c9d73e6683c53fc20e8fdbedbdaaf468982a0c8db2a"}},
                .m_chain_tx_count = 536078574,
                .blockhash = uint256{"00000000000000f4971a7fb37fbdff89315b69a2e1920c467654a382f0d64786"},
            }
        };

        chainTxData = ChainTxData{
            // Data from RPC: getchaintxstats 4096 000000007a61e4230b28ac5cb6b5e5a0130de37ac1faf2f8987d2fa6505b67f4
            .nTime    = 1772051651,
            .tx_count = 536108416,
            .dTxRate  = 0.02691479016257117,
        };

        // Generated by headerssync-params.py on 2026-02-25.
        m_headers_sync_params = HeadersSyncParams{
            .commitment_period = 673,
            .redownload_buffer_size = 14460, // 14460/673 = ~21.5 commitments
        };
    }
};

/**
 * Testnet (v4): public test network which is reset from time to time.
 */
class CTestNet4Params : public CChainParams {
public:
    CTestNet4Params() {
        m_chain_type = ChainType::TESTNET4;
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.nSubsidyHalvingInterval = 210000;
        consensus.BIP34Height = 1;
        consensus.BIP34Hash = uint256{};
        consensus.BIP65Height = 1;
        consensus.BIP66Height = 1;
        consensus.CSVHeight = 1;
        consensus.SegwitHeight = 1;
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256{"00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
        consensus.nPowTargetTimespan = 14 * 24 * 60 * 60; // two weeks
        consensus.nPowTargetSpacing = 10 * 60;
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.enforce_BIP94 = true;
        consensus.fPowNoRetargeting = false;

        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].threshold = 1512; // 75%
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].period = 2016;

        // Deployment of Taproot (BIPs 340-342)
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].bit = 2;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nStartTime = Consensus::BIP9Deployment::ALWAYS_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].min_activation_height = 0; // No activation delay
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].threshold = 1512; // 75%
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].period = 2016;

        consensus.nMinimumChainWork = uint256{"0000000000000000000000000000000000000000000009a0fe15d0177d086304"};
        consensus.defaultAssumeValid = uint256{"0000000002368b1e4ee27e2e85676ae6f9f9e69579b29093e9a82c170bf7cf8a"}; // 123613

        pchMessageStart[0] = 0x1c;
        pchMessageStart[1] = 0x16;
        pchMessageStart[2] = 0x3f;
        pchMessageStart[3] = 0x28;
        nDefaultPort = 48333;
        nPruneAfterHeight = 1000;
        m_assumed_blockchain_size = 31;
        m_assumed_chain_state_size = 2;

        const char* testnet4_genesis_msg = "03/May/2024 000000000000000000001ebd58c244970b3aa9d783bb001011fbe8ea8e98e00e";
        const CScript testnet4_genesis_script = CScript() << "000000000000000000000000000000000000000000000000000000000000000000"_hex << OP_CHECKSIG;
        genesis = CreateGenesisBlock(testnet4_genesis_msg,
                testnet4_genesis_script,
                1714777860,
                393743547,
                0x1d00ffff,
                1,
                BITCOIN_GENESIS_REWARD);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256{"00000000da84f2bafbbc53dee25a72ae507ff4914b867c565be350b0da8bf043"});
        assert(genesis.hashMerkleRoot == uint256{"7aa0a7ae1e223414cb807e40cd57e667b718e42aaf9306db9102fe28912b7b4e"});

        vFixedSeeds.clear();
        vSeeds.clear();
        // nodes with support for servicebits filtering should be at the top
        vSeeds.emplace_back("seed.testnet4.bitcoin.sprovoost.nl."); // Sjors Provoost
        vSeeds.emplace_back("seed.testnet4.wiz.biz."); // Jason Maurice

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,111);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,196);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        bech32_hrp = "tb";

        vFixedSeeds = std::vector<uint8_t>(std::begin(chainparams_seed_testnet4), std::end(chainparams_seed_testnet4));

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;

        m_assumeutxo_data = {
            {
                .height = 90'000,
                .hash_serialized = AssumeutxoHash{uint256{"784fb5e98241de66fdd429f4392155c9e7db5c017148e66e8fdbc95746f8b9b5"}},
                .m_chain_tx_count = 11347043,
                .blockhash = uint256{"0000000002ebe8bcda020e0dd6ccfbdfac531d2f6a81457191b99fc2df2dbe3b"},
            },
            {
                .height = 120'000,
                .hash_serialized = AssumeutxoHash{uint256{"10b05d05ad468d0971162e1b222a4aa66caca89da2bb2a93f8f37fb29c4794b0"}},
                .m_chain_tx_count = 14141057,
                .blockhash = uint256{"000000000bd2317e51b3c5794981c35ba894ce27d3e772d5c39ecd9cbce01dc8"},
            }
        };

        chainTxData = ChainTxData{
            // Data from RPC: getchaintxstats 4096 0000000002368b1e4ee27e2e85676ae6f9f9e69579b29093e9a82c170bf7cf8a
            .nTime    = 1772013387,
            .tx_count = 14191421,
            .dTxRate  = 0.01848579579528412,
        };

        // Generated by headerssync-params.py on 2026-02-25.
        m_headers_sync_params = HeadersSyncParams{
            .commitment_period = 606,
            .redownload_buffer_size = 16092, // 16092/606 = ~26.6 commitments
        };
    }
};

/**
 * Signet: test network with an additional consensus parameter (see BIP325).
 */
class SigNetParams : public CChainParams {
public:
    explicit SigNetParams(const SigNetOptions& options)
    {
        std::vector<uint8_t> bin;
        vFixedSeeds.clear();
        vSeeds.clear();

        if (!options.challenge) {
            bin = "512103ad5e0edad18cb1f0fc0d28a3d4f1f3e445640337489abb10404f2d1e086be430210359ef5021964fe22d6f8e05b2463c9540ce96883fe3b278760f048f5189f2e6c452ae"_hex_v_u8;
            vFixedSeeds = std::vector<uint8_t>(std::begin(chainparams_seed_signet), std::end(chainparams_seed_signet));
            vSeeds.emplace_back("seed.signet.bitcoin.sprovoost.nl.");
            vSeeds.emplace_back("seed.signet.achownodes.xyz."); // Ava Chow, only supports x1, x5, x9, x49, x809, x849, xd, x400, x404, x408, x448, xc08, xc48, x40c

            consensus.nMinimumChainWork = uint256{"00000000000000000000000000000000000000000000000000000b463ea0a4b8"};
            consensus.defaultAssumeValid = uint256{"00000008414aab61092ef93f1aacc54cf9e9f16af29ddad493b908a01ff5c329"}; // 293175
            m_assumed_blockchain_size = 24;
            m_assumed_chain_state_size = 4;
            chainTxData = ChainTxData{
                // Data from RPC: getchaintxstats 4096 00000008414aab61092ef93f1aacc54cf9e9f16af29ddad493b908a01ff5c329
                .nTime    = 1772055248,
                .tx_count = 28676833,
                .dTxRate  = 0.06736623436338929,
            };
        } else {
            bin = *options.challenge;
            consensus.nMinimumChainWork = uint256{};
            consensus.defaultAssumeValid = uint256{};
            m_assumed_blockchain_size = 0;
            m_assumed_chain_state_size = 0;
            chainTxData = ChainTxData{
                0,
                0,
                0,
            };
            LogInfo("Signet with challenge %s", HexStr(bin));
        }

        if (options.seeds) {
            vSeeds = *options.seeds;
        }

        m_chain_type = ChainType::SIGNET;
        consensus.signet_blocks = true;
        consensus.signet_challenge.assign(bin.begin(), bin.end());
        consensus.nSubsidyHalvingInterval = 210000;
        consensus.BIP34Height = 1;
        consensus.BIP34Hash = uint256{};
        consensus.BIP65Height = 1;
        consensus.BIP66Height = 1;
        consensus.CSVHeight = 1;
        consensus.SegwitHeight = 1;
        consensus.nPowTargetTimespan = 14 * 24 * 60 * 60; // two weeks
        consensus.nPowTargetSpacing = 10 * 60;
        consensus.fPowAllowMinDifficultyBlocks = false;
        consensus.enforce_BIP94 = false;
        consensus.fPowNoRetargeting = false;
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256{"00000377ae000000000000000000000000000000000000000000000000000000"};
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].threshold = 1815; // 90%
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].period = 2016;

        // Activation of Taproot (BIPs 340-342)
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].bit = 2;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nStartTime = Consensus::BIP9Deployment::ALWAYS_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].min_activation_height = 0; // No activation delay
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].threshold = 1815; // 90%
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].period = 2016;

        // message start is defined as the first 4 bytes of the sha256d of the block script
        HashWriter h{};
        h << consensus.signet_challenge;
        uint256 hash = h.GetHash();
        std::copy_n(hash.begin(), 4, pchMessageStart.begin());

        nDefaultPort = 38333;
        nPruneAfterHeight = 1000;

        genesis = CreateGenesisBlock(1598918400, 52613770, 0x1e0377ae, 1, BITCOIN_GENESIS_REWARD);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256{"00000008819873e925422c1ff0f99f7cc9bbb232af63a077a480a3633bee1ef6"});
        assert(genesis.hashMerkleRoot == uint256{"4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b"});

        m_assumeutxo_data = {
            {
                .height = 160'000,
                .hash_serialized = AssumeutxoHash{uint256{"fe0a44309b74d6b5883d246cb419c6221bcccf0b308c9b59b7d70783dbdf928a"}},
                .m_chain_tx_count = 2289496,
                .blockhash = uint256{"0000003ca3c99aff040f2563c2ad8f8ec88bd0fd6b8f0895cfaf1ef90353a62c"},
            },
            {
                .height = 290'000,
                .hash_serialized = AssumeutxoHash{uint256{"97267e000b4b876800167e71b9123f1529d13b14308abec2888bbd2160d14545"}},
                .m_chain_tx_count = 28547497,
                .blockhash = uint256{"0000000577f2741bb30cd9d39d6d71b023afbeb9764f6260786a97969d5c9ac0"},
            }
        };

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,111);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,196);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        bech32_hrp = "tb";

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;

        // Generated by headerssync-params.py on 2026-02-25.
        m_headers_sync_params = HeadersSyncParams{
            .commitment_period = 620,
            .redownload_buffer_size = 15724, // 15724/620 = ~25.4 commitments
        };
    }
};

/**
 * Regression test: intended for private networks only. Has minimal difficulty to ensure that
 * blocks can be found instantly.
 */
class CRegTestParams : public CChainParams
{
public:
    explicit CRegTestParams(const RegTestOptions& opts)
    {
        m_chain_type = ChainType::REGTEST;
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.nSubsidyHalvingInterval = 150;
        consensus.BIP34Height = 1; // Always active unless overridden
        consensus.BIP34Hash = uint256();
        consensus.BIP65Height = 1;  // Always active unless overridden
        consensus.BIP66Height = 1;  // Always active unless overridden
        consensus.CSVHeight = 1;    // Always active unless overridden
        consensus.SegwitHeight = 0; // Always active unless overridden
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256{"7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
        consensus.nPowTargetTimespan = 24 * 60 * 60; // one day
        consensus.nPowTargetSpacing = 10 * 60;
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.enforce_BIP94 = opts.enforce_bip94;
        consensus.fPowNoRetargeting = true;

        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].threshold = 108; // 75%
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].period = 144; // Faster than normal for regtest (144 instead of 2016)

        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].bit = 2;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nStartTime = Consensus::BIP9Deployment::ALWAYS_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].min_activation_height = 0; // No activation delay
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].threshold = 108; // 75%
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].period = 144;

        consensus.nMinimumChainWork = uint256{};
        consensus.defaultAssumeValid = uint256{};

        pchMessageStart[0] = 0xfa;
        pchMessageStart[1] = 0xbf;
        pchMessageStart[2] = 0xb5;
        pchMessageStart[3] = 0xda;
        nDefaultPort = 18444;
        nPruneAfterHeight = opts.fastprune ? 100 : 1000;
        m_assumed_blockchain_size = 0;
        m_assumed_chain_state_size = 0;

        for (const auto& [dep, height] : opts.activation_heights) {
            switch (dep) {
            case Consensus::BuriedDeployment::DEPLOYMENT_SEGWIT:
                consensus.SegwitHeight = int{height};
                break;
            case Consensus::BuriedDeployment::DEPLOYMENT_HEIGHTINCB:
                consensus.BIP34Height = int{height};
                break;
            case Consensus::BuriedDeployment::DEPLOYMENT_DERSIG:
                consensus.BIP66Height = int{height};
                break;
            case Consensus::BuriedDeployment::DEPLOYMENT_CLTV:
                consensus.BIP65Height = int{height};
                break;
            case Consensus::BuriedDeployment::DEPLOYMENT_CSV:
                consensus.CSVHeight = int{height};
                break;
            }
        }

        for (const auto& [deployment_pos, version_bits_params] : opts.version_bits_parameters) {
            consensus.vDeployments[deployment_pos].nStartTime = version_bits_params.start_time;
            consensus.vDeployments[deployment_pos].nTimeout = version_bits_params.timeout;
            consensus.vDeployments[deployment_pos].min_activation_height = version_bits_params.min_activation_height;
        }

        genesis = CreateGenesisBlock(1296688602, 2, 0x207fffff, 1, BITCOIN_GENESIS_REWARD);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256{"0f9188f13cb7b2c71f2a335e3a4fc328bf5beb436012afca590b1a11466e2206"});
        assert(genesis.hashMerkleRoot == uint256{"4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b"});

        vFixedSeeds.clear(); //!< Regtest mode doesn't have any fixed seeds.
        vSeeds.clear();
        vSeeds.emplace_back("dummySeed.invalid.");

        fDefaultConsistencyChecks = true;
        m_is_mockable_chain = true;

        m_assumeutxo_data = {
            {   // For use by unit tests
                .height = 110,
                .hash_serialized = AssumeutxoHash{uint256{"b952555c8ab81fec46f3d4253b7af256d766ceb39fb7752b9d18cdf4a0141327"}},
                .m_chain_tx_count = 111,
                .blockhash = uint256{"6affe030b7965ab538f820a56ef56c8149b7dc1d1c144af57113be080db7c397"},
            },
            {
                // For use by fuzz target src/test/fuzz/utxo_snapshot.cpp
                .height = 200,
                .hash_serialized = AssumeutxoHash{uint256{"17dcc016d188d16068907cdeb38b75691a118d43053b8cd6a25969419381d13a"}},
                .m_chain_tx_count = 201,
                .blockhash = uint256{"385901ccbd69dff6bbd00065d01fb8a9e464dede7cfe0372443884f9b1dcf6b9"},
            },
            {
                // For use by test/functional/feature_assumeutxo.py and test/functional/tool_bitcoin_chainstate.py
                .height = 299,
                .hash_serialized = AssumeutxoHash{uint256{"d2b051ff5e8eef46520350776f4100dd710a63447a8e01d917e92e79751a63e2"}},
                .m_chain_tx_count = 334,
                .blockhash = uint256{"7cc695046fec709f8c9394b6f928f81e81fd3ac20977bb68760fa1faa7916ea2"},
            },
        };

        chainTxData = ChainTxData{
            .nTime = 0,
            .tx_count = 0,
            .dTxRate = 0.001, // Set a non-zero rate to make it testable
        };

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,111);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,196);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        bech32_hrp = "bcrt";

        // Copied from Testnet4.
        m_headers_sync_params = HeadersSyncParams{
            .commitment_period = 275,
            .redownload_buffer_size = 7017, // 7017/275 = ~25.5 commitments
        };
    }
};

std::unique_ptr<const CChainParams> CChainParams::SigNet(const SigNetOptions& options)
{
    return std::make_unique<const SigNetParams>(options);
}

std::unique_ptr<const CChainParams> CChainParams::RegTest(const RegTestOptions& options)
{
    return std::make_unique<const CRegTestParams>(options);
}

std::unique_ptr<const CChainParams> CChainParams::Main()
{
    return std::make_unique<const CMainParams>();
}

std::unique_ptr<const CChainParams> CChainParams::TestNet()
{
    return std::make_unique<const CTestNetParams>();
}

std::unique_ptr<const CChainParams> CChainParams::TestNet4()
{
    return std::make_unique<const CTestNet4Params>();
}

std::vector<int> CChainParams::GetAvailableSnapshotHeights() const
{
    std::vector<int> heights;
    heights.reserve(m_assumeutxo_data.size());

    for (const auto& data : m_assumeutxo_data) {
        heights.emplace_back(data.height);
    }
    return heights;
}

std::optional<ChainType> GetNetworkForMagic(const MessageStartChars& message)
{
    const auto mainnet_msg = CChainParams::Main()->MessageStart();
    const auto testnet_msg = CChainParams::TestNet()->MessageStart();
    const auto testnet4_msg = CChainParams::TestNet4()->MessageStart();
    const auto regtest_msg = CChainParams::RegTest({})->MessageStart();
    const auto signet_msg = CChainParams::SigNet({})->MessageStart();

    if (std::ranges::equal(message, mainnet_msg)) {
        return ChainType::MAIN;
    } else if (std::ranges::equal(message, testnet_msg)) {
        return ChainType::TESTNET;
    } else if (std::ranges::equal(message, testnet4_msg)) {
        return ChainType::TESTNET4;
    } else if (std::ranges::equal(message, regtest_msg)) {
        return ChainType::REGTEST;
    } else if (std::ranges::equal(message, signet_msg)) {
        return ChainType::SIGNET;
    }
    return std::nullopt;
}
