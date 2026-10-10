// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Bridge V2 B-37 (b3-bridge-v2-finality-dependency.md section 6.4; node
// changes section 7.7), wallet side: a fresh-lineage key comes from fresh
// randomness into the node's fresh key store, never from the wallet's
// identity secret and never into the wallet file. So a wallet backup holds no
// fresh secret, a wallet restored from it cannot produce one, and two wallets
// sharing one identity generate unrelated keys -- unlike V1's
// DeriveFinalityBlsKey, which an old backup re-derives. The binding the
// wallet signs (BIP340 identity over the FRESHBIND digest + PoP) is checked;
// without the regtest -b3freshkeyheight knob the new RPCs refuse or report
// configured=false, and with it getfreshfinalitykeys lists the node store's
// public records (never a secret) and reports a store that fails closed.

#include <chainparams.h>
#include <common/args.h>
#include <crypto/bls.h>
#include <interfaces/chain.h>
#include <key.h>
#include <consensus/params.h>
#include <modern/chain_domain.h>
#include <modern/finality_fresh_key.h>
#include <modern/mpa.h>
#include <node/finality_fresh_key_store.h>
#include <pubkey.h>
#include <rpc/protocol.h>
#include <rpc/request.h>
#include <rpc/util.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <univalue.h>
#include <util/chaintype.h>
#include <util/fs.h>
#include <util/translation.h>
#include <validation.h>
#include <wallet/context.h>
#include <wallet/rpc/finality_fresh_key.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>
#include <wallet/walletutil.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace wallet {
RPCHelpMan bindfreshfinalitykey();
RPCHelpMan getfreshfinalitykeys();

namespace {

const uint256 CHAIN_DOMAIN{uint256{"d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0"}};
const uint256 OTHER_CHAIN_DOMAIN{uint256{"d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1"}};

std::vector<unsigned char> ReadFileBytes(const fs::path& path)
{
    std::ifstream file{path.std_path(), std::ios::binary};
    BOOST_REQUIRE_MESSAGE(file.good(), "cannot read " << fs::PathToString(path));
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

bool Contains(const std::vector<unsigned char>& haystack, std::span<const unsigned char> needle)
{
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) != haystack.end();
}

modern::ValidatorKeyBytes XOnly(const CPubKey& pubkey)
{
    const XOnlyPubKey xonly{pubkey};
    modern::ValidatorKeyBytes out{};
    std::copy(xonly.begin(), xonly.end(), out.begin());
    return out;
}

modern::BlsPubkeyBytes Pk(const bls::SecretKey& key) { return key.GetPublicKey().Compressed(); }

//! Real SQLite wallets in the test wallet directory, attached to the chain.
struct FreshKeyWalletSetup : public TestingSetup {
    WalletContext context;
    const bool previous_doc_check;

    FreshKeyWalletSetup()
        : TestingSetup{ChainType::REGTEST}, previous_doc_check{gArgs.GetBoolArg("-rpcdoccheck", DEFAULT_RPC_DOC_CHECK)}
    {
        m_args.ForceSetArg("-unsafesqlitesync", "1");
        gArgs.ForceSetArg("-rpcdoccheck", "1");
        context.args = m_node.args;
        context.chain = m_node.chain.get();
    }
    ~FreshKeyWalletSetup() { gArgs.ForceSetArg("-rpcdoccheck", previous_doc_check ? "1" : "0"); }

    std::shared_ptr<CWallet> Create(const std::string& name)
    {
        DatabaseOptions options;
        options.require_create = true;
        options.create_flags = WALLET_FLAG_DESCRIPTORS;
        DatabaseStatus status;
        bilingual_str error;
        std::vector<bilingual_str> warnings;
        auto database{MakeWalletDatabase(name, options, status, error)};
        BOOST_REQUIRE_MESSAGE(database, error.original);
        auto wallet{CWallet::CreateNew(context, name, std::move(database), options.create_flags, error, warnings)};
        BOOST_REQUIRE_MESSAGE(wallet, error.original);
        return wallet;
    }
    //! Restore `backup` as wallet `name` (RestoreWallet's copy) and load it.
    std::shared_ptr<CWallet> Restore(const fs::path& backup, const std::string& name)
    {
        DatabaseStatus status;
        bilingual_str error;
        std::vector<bilingual_str> warnings;
        RestoreWallet(context, backup, name, /*load_on_start=*/std::nullopt, status, error, warnings,
                      /*load_after_restore=*/false);
        BOOST_REQUIRE_MESSAGE(error.empty(), error.original);
        DatabaseOptions options;
        options.require_existing = true;
        auto database{MakeWalletDatabase(name, options, status, error)};
        BOOST_REQUIRE_MESSAGE(database, error.original);
        auto wallet{CWallet::LoadExisting(context, name, std::move(database), error, warnings)};
        BOOST_REQUIRE_MESSAGE(wallet, error.original);
        return wallet;
    }
    UniValue Call(RPCHelpMan method, UniValue params = UniValue{UniValue::VARR})
    {
        JSONRPCRequest request;
        request.context = &context;
        request.strMethod = method.m_name;
        request.params = std::move(params);
        return method.HandleRequest(request);
    }
    //! The message of a call expected to fail with `code`.
    std::string Error(RPCHelpMan method, UniValue params, const int code)
    {
        try {
            Call(std::move(method), std::move(params));
        } catch (const UniValue& error) {
            BOOST_CHECK_EQUAL(error.find_value("code").getInt<int>(), code);
            return error.find_value("message").get_str();
        }
        BOOST_FAIL("expected a JSON-RPC error");
        return "";
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(finality_fresh_key_wallet_tests, FreshKeyWalletSetup)

BOOST_AUTO_TEST_CASE(binding_parts_carry_identity_domain_and_epoch)
{
    CKey identity;
    identity.MakeNewKey(/*fCompressed=*/true);
    const modern::ValidatorKeyBytes vk{XOnly(identity.GetPubKey())};
    std::array<unsigned char, 32> ikm{};
    ikm.fill(0x37);
    const bls::SecretKey key{*bls::SecretKey::FromIKM(ikm)};

    const auto parts{MakeFreshFinalityKeyBinding(identity, CHAIN_DOMAIN, key, 2)};
    BOOST_REQUIRE_MESSAGE(parts, util::ErrorString(parts).original);
    BOOST_CHECK(parts->params.bls_pubkey == Pk(key));
    BOOST_CHECK_EQUAL(parts->params.target_epoch, 2U);
    BOOST_CHECK(parts->evidence.validator_key == vk);
    BOOST_CHECK(parts->evidence.bls_pubkey == Pk(key));
    BOOST_CHECK_EQUAL(parts->evidence.target_epoch, 2U);
    // The cell and the type-12 record are exactly the modern encodings.
    BOOST_CHECK(parts->cell == modern::MakeFreshFinalityKeyCell(vk, parts->params));
    BOOST_CHECK_EQUAL(parts->record.payload_type, modern::MPA_TYPE_FRESH_FINALITY_KEY_EVIDENCE);
    BOOST_CHECK(parts->record == modern::MakeFreshFinalityKeyRecord(parts->evidence));
    uint256 commitment;
    std::copy(vk.begin(), vk.end(), commitment.begin());
    BOOST_CHECK(modern::CheckFreshKeyTransition(CHAIN_DOMAIN, commitment, parts->params, parts->evidence) ==
                modern::FreshKeyCheck::OK);
    // The identity signature names the chain domain and the epoch.
    BOOST_CHECK(modern::CheckFreshKeyTransition(OTHER_CHAIN_DOMAIN, commitment, parts->params, parts->evidence) ==
                modern::FreshKeyCheck::BAD_BIP340_SIGNATURE);
    {
        auto other_epoch{parts->params};
        auto other_evidence{parts->evidence};
        other_epoch.target_epoch = other_evidence.target_epoch = 3;
        BOOST_CHECK(modern::CheckFreshKeyTransition(CHAIN_DOMAIN, commitment, other_epoch, other_evidence) ==
                    modern::FreshKeyCheck::BAD_BIP340_SIGNATURE);
    }
    // The proof of possession is identity-bound, never the bare IETF PoP:
    // it verifies only for this validator, chain and epoch.
    BOOST_CHECK(parts->evidence.pop != key.SignPoP().Compressed());
    {
        const uint256 pop_digest{modern::FreshFinalityPopDigest(CHAIN_DOMAIN, vk, Pk(key), 2)};
        const auto pop{bls::Signature::Decode(parts->evidence.pop)};
        BOOST_REQUIRE(pop);
        BOOST_CHECK(bls::Verify(key.GetPublicKey(), std::span<const unsigned char>(pop_digest.begin(), 32), *pop));
        BOOST_CHECK(!bls::VerifyPoP(key.GetPublicKey(), *pop));
    }
    // No chain domain, no binding.
    BOOST_CHECK(!MakeFreshFinalityKeyBinding(identity, uint256{}, key, 2));
}

BOOST_AUTO_TEST_CASE(fresh_keys_are_independent_of_the_wallet_and_its_backups)
{
    // This node's signer directory and another node's (where a restored
    // backup of the same wallet runs).
    const fs::path signer_dir{gArgs.GetDataDirNet() / node::FINALITY_SIGNER_DIRNAME};
    const fs::path other_signer_dir{m_path_root / "other-node" / node::FINALITY_SIGNER_DIRNAME};
    std::shared_ptr<CWallet> wallet{Create("fresh-a")};

    // 1. The wallet's identity and every V1 key an old backup re-derives:
    //    DeriveFinalityBlsKey(0..15) and an imported independent key.
    modern::ValidatorKeyBytes vk{};
    CKey identity;
    std::vector<bls::SecretKey> v1_keys;
    {
        LOCK(wallet->cs_wallet);
        const auto validator{wallet->GetOrCreateValidatorKey()};
        BOOST_REQUIRE_MESSAGE(validator, util::ErrorString(validator).original);
        vk = XOnly(*validator);
        const auto secret{wallet->GetValidatorSecret()};
        BOOST_REQUIRE(secret);
        identity = *secret;
        for (uint32_t seq{0}; seq < 16; ++seq) {
            const auto derived{wallet->DeriveFinalityBlsKey(seq)};
            BOOST_REQUIRE(derived);
            v1_keys.push_back(*derived);
        }
        std::array<unsigned char, 32> ikm{};
        ikm.fill(0x5A);
        const auto independent{bls::SecretKey::FromIKM(ikm)};
        BOOST_REQUIRE(independent && wallet->ImportFinalityBlsKey(*independent));
        v1_keys.push_back(*independent);
    }

    // 2. Fresh keys from the store (the bindfreshfinalitykey path): random,
    //    none equal to a V1 key, one per epoch, durable in the store file.
    node::FreshKeyStore store;
    const auto fresh0{GenerateFreshFinalityKey(store, signer_dir, CHAIN_DOMAIN, vk, 0)};
    BOOST_REQUIRE_MESSAGE(fresh0, util::ErrorString(fresh0).original);
    const auto fresh2{GenerateFreshFinalityKey(store, signer_dir, CHAIN_DOMAIN, vk, 2)};
    BOOST_REQUIRE_MESSAGE(fresh2, util::ErrorString(fresh2).original);
    const std::vector<bls::SecretKey> fresh_keys{*fresh0, *fresh2};
    BOOST_CHECK(Pk(*fresh0) != Pk(*fresh2));
    for (const bls::SecretKey& fresh : fresh_keys) {
        for (const bls::SecretKey& v1 : v1_keys) {
            BOOST_CHECK(Pk(fresh) != Pk(v1));
            BOOST_CHECK(fresh.Bytes() != v1.Bytes());
        }
    }
    {
        const auto again{GenerateFreshFinalityKey(store, signer_dir, CHAIN_DOMAIN, vk, 0)};
        BOOST_REQUIRE(!again);
        BOOST_CHECK_MESSAGE(util::ErrorString(again).original.find("already holds key " + HexStr(Pk(*fresh0))) !=
                                std::string::npos,
                            util::ErrorString(again).original);
    }
    const fs::path store_path{node::FreshKeyStore::StatePath(signer_dir, CHAIN_DOMAIN, vk)};
    for (const bls::SecretKey& fresh : fresh_keys) {
        BOOST_CHECK(Contains(ReadFileBytes(store_path), fresh.Bytes())); // the only copy
    }
    {
        // The identity authorizes a binding of the fresh key...
        const auto parts{MakeFreshFinalityKeyBinding(identity, CHAIN_DOMAIN, *fresh0, 0)};
        BOOST_REQUIRE(parts);
        BOOST_CHECK(parts->evidence.validator_key == vk);
    }

    // 3. A backup holds the wallet's V1 imported secret (the scan finds what
    //    the wallet holds) and no fresh secret or fresh public key; neither
    //    does the live wallet file.
    const fs::path backup{m_path_root / "fresh-a.backup"};
    BOOST_REQUIRE(wallet->BackupWallet(fs::PathToString(backup)));
    const std::vector<unsigned char> backup_bytes{ReadFileBytes(backup)};
    BOOST_CHECK(Contains(backup_bytes, v1_keys.back().Bytes()));
    const std::vector<unsigned char> wallet_bytes{ReadFileBytes(GetWalletDir() / "fresh-a" / "wallet.dat")};
    for (const bls::SecretKey& fresh : fresh_keys) {
        for (const std::vector<unsigned char>* bytes : {&backup_bytes, &wallet_bytes}) {
            BOOST_CHECK(!Contains(*bytes, fresh.Bytes()));
            BOOST_CHECK(!Contains(*bytes, Pk(fresh)));
        }
    }

    // 4. The wallet restored from that backup has the same identity and
    //    re-derives every V1 key, yet produces no fresh key: none of its keys
    //    match, its node's store is empty, and generating there for the same
    //    epoch yields an unrelated key. On the original node the shared
    //    (domain, validator) store still refuses a second key for epoch 0.
    std::shared_ptr<CWallet> restored{Restore(backup, "fresh-a-restored")};
    {
        LOCK(restored->cs_wallet);
        const auto validator{restored->GetValidatorPubKey()};
        BOOST_REQUIRE(validator);
        BOOST_CHECK(XOnly(*validator) == vk);
        for (uint32_t seq{0}; seq < 16; ++seq) {
            const auto derived{restored->DeriveFinalityBlsKey(seq)};
            BOOST_REQUIRE(derived);
            BOOST_CHECK(Pk(*derived) == Pk(v1_keys[seq]));
            for (const bls::SecretKey& fresh : fresh_keys) BOOST_CHECK(Pk(*derived) != Pk(fresh));
        }
        const auto imported{restored->GetImportedFinalityBlsKey()};
        BOOST_REQUIRE(imported);
        BOOST_CHECK(imported->Bytes() == v1_keys.back().Bytes());
    }
    {
        node::FreshKeyStore other;
        std::string error;
        BOOST_REQUIRE_MESSAGE(other.Open(other_signer_dir, CHAIN_DOMAIN, vk, error), error);
        BOOST_CHECK(other.IsAbsent());
        for (const bls::SecretKey& fresh : fresh_keys) {
            BOOST_CHECK(!other.RecordFor(Pk(fresh)));
            BOOST_CHECK(!other.SecretFor(Pk(fresh)));
        }
        const auto unrelated{GenerateFreshFinalityKey(other, other_signer_dir, CHAIN_DOMAIN, vk, 0)};
        BOOST_REQUIRE_MESSAGE(unrelated, util::ErrorString(unrelated).original);
        for (const bls::SecretKey& fresh : fresh_keys) BOOST_CHECK(Pk(*unrelated) != Pk(fresh));
        for (const bls::SecretKey& v1 : v1_keys) BOOST_CHECK(Pk(*unrelated) != Pk(v1));
    }
    {
        node::FreshKeyStore same_node;
        const auto refused{GenerateFreshFinalityKey(same_node, signer_dir, CHAIN_DOMAIN, vk, 0)};
        BOOST_CHECK(!refused);
    }

    // 5. An erased fresh key is gone from the only place that held it.
    {
        std::string error;
        BOOST_REQUIRE_MESSAGE(store.Open(signer_dir, CHAIN_DOMAIN, vk, error), error);
        BOOST_REQUIRE_MESSAGE(store.Erase(Pk(*fresh0), error), error);
        BOOST_CHECK(!Contains(ReadFileBytes(store_path), fresh0->Bytes()));
        BOOST_CHECK(Contains(ReadFileBytes(store_path), fresh2->Bytes()));
    }

    TestUnloadWallet(std::move(restored));
    TestUnloadWallet(std::move(wallet));
}

BOOST_AUTO_TEST_CASE(rpcs_refuse_or_report_without_key_mode)
{
    // Regtest without -b3freshkeyheight (as on every shipped network): the
    // chain reports configured = false, with the signer directory init.cpp
    // hands the staking loop.
    BOOST_REQUIRE(!Params().GetConsensus().fresh_lineage);
    {
        const std::array<unsigned char, 32> any_key{};
        const interfaces::FreshFinalityKeyStatus status{m_node.chain->freshFinalityKeyStatus(any_key, {})};
        BOOST_CHECK(!status.configured);
        BOOST_CHECK(!status.available);
        BOOST_CHECK(status.bindings.empty() && status.seats.empty());
        BOOST_CHECK(status.signer_directory == gArgs.GetDataDirNet() / "finality_signer");
    }

    std::shared_ptr<CWallet> wallet{Create("fresh-rpc")};
    WITH_LOCK(context.wallets_mutex, context.wallets.push_back(wallet));
    // No validator key yet: the status RPC creates none.
    BOOST_CHECK(Error(getfreshfinalitykeys(), UniValue{UniValue::VARR}, RPC_WALLET_ERROR).find("no validator key") !=
                std::string::npos);
    // Option checks come first.
    {
        UniValue params{UniValue::VARR};
        params.push_back(UniValue{});
        UniValue options{UniValue::VOBJ};
        options.pushKV("reuse_key", "v1");
        params.push_back(options);
        BOOST_CHECK_EQUAL(Error(bindfreshfinalitykey(), params, RPC_INVALID_PARAMETER),
                          "reuse_key requires broadcast=false");
    }
    BOOST_CHECK(Error(bindfreshfinalitykey(), UniValue{UniValue::VARR}, RPC_MISC_ERROR)
                    .find("Fresh-lineage per-epoch keys (B-37) are not configured on this chain") != std::string::npos);
    // The refusal comes before the wallet is touched: no validator identity
    // was created (nothing written to the wallet), and no store file.
    BOOST_CHECK(!WITH_LOCK(wallet->cs_wallet, return wallet->GetValidatorPubKey().has_value()));
    BOOST_CHECK(!fs::exists(gArgs.GetDataDirNet() / "finality_signer"));
    // With an identity the refusal is the same.
    BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return bool{wallet->GetOrCreateValidatorKey()}));
    BOOST_CHECK(Error(bindfreshfinalitykey(), UniValue{UniValue::VARR}, RPC_MISC_ERROR)
                    .find("Fresh-lineage per-epoch keys (B-37) are not configured on this chain") != std::string::npos);
    BOOST_CHECK(!fs::exists(gArgs.GetDataDirNet() / "finality_signer"));
    BOOST_CHECK_EQUAL(Call(getfreshfinalitykeys()).write(), R"({"configured":false})");

    WITH_LOCK(context.wallets_mutex, context.wallets.clear());
    TestUnloadWallet(std::move(wallet));
}

BOOST_AUTO_TEST_CASE(status_rpc_reads_the_node_store_in_key_mode)
{
    // Key mode configured on this regtest chain (fresh lineage + R_key, a
    // pinned boundary hash for the chain domain) but no modern chain under
    // it: the finality state is not derivable, so no window is open and
    // binding is refused, while getfreshfinalitykeys still lists the records
    // of this validator's store in the node's signer directory.
    auto& consensus{const_cast<Consensus::Params&>(m_node.chainman->GetConsensus())};
    consensus.legacy_final_hash = uint256::ONE;
    consensus.fresh_lineage = Consensus::FreshLineageParams{
        .activation_height = 262, .max_epoch_extension = 1280, .staging_signer = true};
    consensus.fresh_lineage->key_height = 241;
    const auto domain{modern::ModernChainDomain(consensus.hashGenesisBlock, *consensus.legacy_final_hash)};
    BOOST_REQUIRE(domain);
    const fs::path signer_dir{gArgs.GetDataDirNet() / node::FINALITY_SIGNER_DIRNAME};

    std::shared_ptr<CWallet> wallet{Create("fresh-status")};
    WITH_LOCK(context.wallets_mutex, context.wallets.push_back(wallet));
    // A wallet without a validator identity gets none from a refused bind
    // (here the chain state is refused first; with it available the RPC
    // answers "no validator key yet": a seat needs a stake and a V1 binding).
    BOOST_CHECK(Error(bindfreshfinalitykey(), UniValue{UniValue::VARR}, RPC_MISC_ERROR)
                    .find("The fresh key state is unavailable at the tip") != std::string::npos);
    BOOST_CHECK(!WITH_LOCK(wallet->cs_wallet, return wallet->GetValidatorPubKey().has_value()));
    modern::ValidatorKeyBytes vk{};
    {
        LOCK(wallet->cs_wallet);
        const auto validator{wallet->GetOrCreateValidatorKey()};
        BOOST_REQUIRE(validator);
        vk = XOnly(*validator);
    }
    const interfaces::FreshFinalityKeyStatus status{m_node.chain->freshFinalityKeyStatus(vk, {})};
    BOOST_CHECK(status.configured && !status.available);
    BOOST_CHECK(status.chain_domain == *domain);
    BOOST_CHECK(status.key_height == 241);
    BOOST_CHECK_EQUAL(status.activation_height, 262);
    BOOST_CHECK_EQUAL(status.next_binding_window, "not-active");
    BOOST_CHECK(status.signer_directory == signer_dir);
    BOOST_CHECK(Error(bindfreshfinalitykey(), UniValue{UniValue::VARR}, RPC_MISC_ERROR)
                    .find("The fresh key state is unavailable at the tip") != std::string::npos);
    BOOST_CHECK(!fs::exists(signer_dir)); // the refusal generated nothing

    // Two keys in this validator's store (one with its binding transaction).
    node::FreshKeyStore store;
    const auto key0{GenerateFreshFinalityKey(store, signer_dir, *domain, vk, 0)};
    const auto key2{GenerateFreshFinalityKey(store, signer_dir, *domain, vk, 2)};
    BOOST_REQUIRE(key0 && key2);
    const uint256 txid{uint256{"7777777777777777777777777777777777777777777777777777777777777777"}};
    std::string error;
    BOOST_REQUIRE_MESSAGE(store.SetBindTxid(Pk(*key0), txid, error), error);
    BOOST_REQUIRE_MESSAGE(store.Erase(Pk(*key2), error), error);

    // Replacement policy (bindfreshfinalitykey): a held key for the epoch
    // blocks a new one unless the policy gives a reason; a replaced key is
    // erased first and reported. Here: epoch 5, first without a binding
    // transaction (never published), then with one (kept).
    {
        node::FreshKeyStore scratch;
        const fs::path scratch_dir{gArgs.GetDataDirNet() / "replace_policy"};
        const auto unbound{GenerateFreshFinalityKey(scratch, scratch_dir, *domain, vk, 5)};
        BOOST_REQUIRE(unbound);
        const FreshKeyReplacePolicy never_published{[](const node::FreshKeyRecord& r) -> std::optional<std::string> {
            if (r.bind_txid.IsNull()) return std::string{"never published"};
            return std::nullopt;
        }};
        BOOST_CHECK(!GenerateFreshFinalityKey(scratch, scratch_dir, *domain, vk, 5)); // no policy: refused
        std::vector<std::string> replaced;
        const auto replacement{GenerateFreshFinalityKey(scratch, scratch_dir, *domain, vk, 5, never_published, &replaced)};
        BOOST_REQUIRE_MESSAGE(replacement, util::ErrorString(replacement).original);
        BOOST_CHECK(Pk(*replacement) != Pk(*unbound));
        BOOST_REQUIRE_EQUAL(replaced.size(), 1U);
        BOOST_CHECK_EQUAL(replaced[0], HexStr(Pk(*unbound)) + ": never published");
        BOOST_REQUIRE(scratch.RecordFor(Pk(*unbound)));
        BOOST_CHECK(!scratch.RecordFor(Pk(*unbound))->Held());
        BOOST_CHECK(!scratch.SecretFor(Pk(*unbound)));
        BOOST_REQUIRE_MESSAGE(scratch.SetBindTxid(Pk(*replacement), txid, error), error);
        BOOST_CHECK(!GenerateFreshFinalityKey(scratch, scratch_dir, *domain, vk, 5, never_published, &replaced));
        BOOST_CHECK_EQUAL(replaced.size(), 1U);
        BOOST_CHECK(scratch.SecretFor(Pk(*replacement)));
    }

    const UniValue result{Call(getfreshfinalitykeys())};
    BOOST_CHECK(result["configured"].get_bool());
    BOOST_CHECK_EQUAL(result["validator_key"].get_str(), HexStr(vk));
    BOOST_CHECK_EQUAL(result["key_height"].getInt<int>(), 241);
    BOOST_CHECK_EQUAL(result["activation_height"].getInt<int>(), 262);
    BOOST_CHECK_EQUAL(result["chain_domain"].get_str(), domain->GetHex());
    BOOST_CHECK(!result["state_available"].get_bool());
    BOOST_CHECK_EQUAL(result["next_binding_window"].get_str(), "not-active");
    BOOST_CHECK(result.find_value("next_binding_epoch").isNull());
    BOOST_CHECK(!result["bootstrapped"].get_bool());
    BOOST_CHECK_EQUAL(result["store_path"].get_str(),
                      fs::PathToString(node::FreshKeyStore::StatePath(signer_dir, *domain, vk)));
    BOOST_CHECK(result.find_value("store_error").isNull());
    const UniValue& keys{result["keys"]};
    BOOST_REQUIRE_EQUAL(keys.size(), 2U);
    BOOST_CHECK_EQUAL(keys[0]["epoch"].getInt<int>(), 0);
    BOOST_CHECK_EQUAL(keys[0]["bls_pubkey"].get_str(), HexStr(Pk(*key0)));
    BOOST_CHECK_EQUAL(keys[0]["state"].get_str(), "held");
    BOOST_CHECK_EQUAL(keys[0]["bind_txid"].get_str(), txid.GetHex());
    BOOST_CHECK(keys[0].find_value("confirmations").isNull()); // not a transaction of this wallet
    BOOST_CHECK(keys[0].find_value("counted").isNull());       // no chain binding reported
    BOOST_CHECK(keys[0]["seated_epochs"].empty());
    BOOST_CHECK_EQUAL(keys[1]["epoch"].getInt<int>(), 2);
    BOOST_CHECK_EQUAL(keys[1]["state"].get_str(), "erased");
    BOOST_CHECK(keys[1].find_value("bind_txid").isNull());
    BOOST_CHECK(result["foreign_bindings"].empty());
    BOOST_CHECK(result["missing_seats"].empty());
    BOOST_CHECK(result["alerts"].empty());
    // The RPCs never reveal a secret.
    for (const bls::SecretKey* key : {&*key0}) {
        BOOST_CHECK(result.write().find(HexStr(key->Bytes())) == std::string::npos);
    }

    // A store that fails closed is reported, with no foreign-binding guess.
    {
        const fs::path path{node::FreshKeyStore::StatePath(signer_dir, *domain, vk)};
        std::vector<unsigned char> bytes{ReadFileBytes(path)};
        bytes.back() ^= 1;
        std::ofstream out{path.std_path(), std::ios::binary | std::ios::trunc};
        out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    const UniValue broken{Call(getfreshfinalitykeys())};
    BOOST_CHECK(!broken["store_error"].get_str().empty());
    BOOST_CHECK(broken["keys"].empty());
    BOOST_REQUIRE_EQUAL(broken["alerts"].size(), 1U);
    BOOST_CHECK(broken["alerts"][0].get_str().find("cannot read the fresh key store") != std::string::npos);

    WITH_LOCK(context.wallets_mutex, context.wallets.clear());
    TestUnloadWallet(std::move(wallet));
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace wallet
