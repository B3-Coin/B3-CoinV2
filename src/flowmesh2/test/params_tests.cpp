// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> defines DOMAIN (ARCHITECTURE.md O-21)

#include <boost/test/unit_test.hpp>

#include <crypto/hex_base.h>
#include <crypto/sha256.h>
#include <flowmesh2/core/params.h>
#include <univalue.h>
#include <util/strencodings.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace flowmesh2;

namespace {

std::string Hex(const uint256& value) { return HexStr(value); }

ProfileParams Dev()
{
    auto profile{DevRegtestProfile(ChainType::REGTEST)};
    BOOST_REQUIRE(profile.has_value());
    return *profile;
}

//! Synthetic TEST network: magic "FM2T", domain SHA256("fm2/test/domain").
NetworkId TestNetwork()
{
    NetworkId network;
    network.chain = ChainType::REGTEST;
    network.magic = {0x46, 0x4d, 0x32, 0x54};
    const std::string label{"fm2/test/domain"};
    CSHA256{}.Write(reinterpret_cast<const unsigned char*>(label.data()), label.size()).Finalize(network.modern_chain_domain.data());
    return network;
}

void CheckCode(const Result<void>& result, ErrorCode expected, std::string_view detail = {})
{
    BOOST_REQUIRE_MESSAGE(!result.has_value(), "unexpected success");
    BOOST_CHECK_EQUAL(ToString(result.error().code), ToString(expected));
    if (!detail.empty()) BOOST_CHECK_EQUAL(result.error().detail, detail);
}

//! One mutation per ProfileParams field. field_count_matches_struct below
//! fails to compile if a field is added without a mutation here.
std::vector<std::pair<std::string, std::function<void(ProfileParams&)>>> FieldMutations()
{
    return {
        {"profile_id", [](ProfileParams& p) { p.profile_id += "x"; }},
        {"fm_version", [](ProfileParams& p) { ++p.fm_version; }},
        {"chain", [](ProfileParams& p) { p.chain = ChainType::SIGNET; }},
        {"matching", [](ProfileParams& p) { p.matching = static_cast<MatchingProfile>(2); }},
        {"fee_ppm", [](ProfileParams& p) { ++p.fee_ppm; }},
        {"fee_incidence", [](ProfileParams& p) { p.fee_incidence = FeeIncidence::PER_SIDE; }},
        {"seat_share_percent", [](ProfileParams& p) { ++p.seat_share_percent; }},
        {"max_actions_per_slot", [](ProfileParams& p) { ++p.max_actions_per_slot; }},
        {"max_action_payload_bytes", [](ProfileParams& p) { ++p.max_action_payload_bytes; }},
        {"max_fills_per_order", [](ProfileParams& p) { ++p.max_fills_per_order; }},
        {"max_fills_per_slot", [](ProfileParams& p) { ++p.max_fills_per_slot; }},
        {"max_changed_leaves_per_slot", [](ProfileParams& p) { ++p.max_changed_leaves_per_slot; }},
        {"max_open_orders_per_account", [](ProfileParams& p) { ++p.max_open_orders_per_account; }},
        {"price_band_enabled", [](ProfileParams& p) { p.price_band_enabled = !p.price_band_enabled; }},
        {"price_band_percent", [](ProfileParams& p) { ++p.price_band_percent; }},
        {"nonce_window", [](ProfileParams& p) { ++p.nonce_window; }},
        {"max_delay_slots", [](ProfileParams& p) { ++p.max_delay_slots; }},
        {"futures_enabled", [](ProfileParams& p) { p.futures_enabled = !p.futures_enabled; }},
        {"signer_mode", [](ProfileParams& p) { p.signer_mode = SignerMode::FOLDED_SIGNING; }},
        {"pool_capacity", [](ProfileParams& p) { ++p.pool_capacity; }},
        {"outcome_index_slots", [](ProfileParams& p) { ++p.outcome_index_slots; }},
        {"stable_fee_asset", [](ProfileParams& p) { p.stable_fee_asset = RegtestQuoteAsset(); }},
        {"withdrawals_enabled", [](ProfileParams& p) { p.withdrawals_enabled = true; }},
        {"withdrawal_fee_atoms", [](ProfileParams& p) { p.withdrawal_fee_atoms = 1; }},
        {"view_bound_commits", [](ProfileParams& p) { p.view_bound_commits = true; }},
        {"source_range_commitment", [](ProfileParams& p) { p.source_range_commitment = true; }},
        {"source_chain_id", [](ProfileParams& p) { p.source_chain_id = 31337; }},
        {"source_confirmations", [](ProfileParams& p) { p.source_confirmations = 2; }},
    };
}

constexpr size_t LEGACY_PROFILE_FIELDS{21};
constexpr size_t PROFILE_FIELDS{28};

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream file{path, std::ios::binary};
    BOOST_REQUIRE_MESSAGE(file.good(), "cannot open " << path.string());
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

} // namespace

BOOST_AUTO_TEST_SUITE(params_tests)

BOOST_AUTO_TEST_CASE(field_count_matches_struct)
{
    // A structured binding needs exactly one name per field, so adding or
    // removing a ProfileParams field breaks this line until FieldMutations,
    // EncodeProfile, the JSON file and PROFILE_FIELDS are updated with it.
    const ProfileParams p{Dev()};
    const auto& [f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28] = p;
    (void)f1, (void)f2, (void)f3, (void)f4, (void)f5, (void)f6, (void)f7, (void)f8, (void)f9, (void)f10, (void)f11;
    (void)f12, (void)f13, (void)f14, (void)f15, (void)f16, (void)f17, (void)f18, (void)f19, (void)f20, (void)f21, (void)f22, (void)f23, (void)f24, (void)f25, (void)f26, (void)f27, (void)f28;
    BOOST_CHECK_EQUAL(FieldMutations().size(), PROFILE_FIELDS);
}

BOOST_AUTO_TEST_CASE(dev_profile_values_pinned)
{
    const ProfileParams p{Dev()};
    BOOST_CHECK_EQUAL(p.profile_id, "fm2-dex1-regtest-provisional/1");
    BOOST_CHECK_EQUAL(p.fm_version, 1U);
    BOOST_CHECK(p.chain == ChainType::REGTEST);
    BOOST_CHECK(p.matching == MatchingProfile::A2_PRORATA);
    BOOST_CHECK_EQUAL(p.fee_ppm, 100U);
    BOOST_CHECK(p.fee_incidence == FeeIncidence::SELLER);
    BOOST_CHECK_EQUAL(p.seat_share_percent, 80U);
    BOOST_CHECK_EQUAL(p.max_actions_per_slot, 1024U);
    BOOST_CHECK_EQUAL(p.max_action_payload_bytes, 4096U);
    BOOST_CHECK_EQUAL(p.max_fills_per_order, 1024U);
    BOOST_CHECK_EQUAL(p.max_fills_per_slot, 16384U);
    BOOST_CHECK_EQUAL(p.max_changed_leaves_per_slot, 8192U);
    BOOST_CHECK_EQUAL(p.max_open_orders_per_account, 1000U);
    BOOST_CHECK(!p.price_band_enabled);
    BOOST_CHECK_EQUAL(p.price_band_percent, 50U);
    BOOST_CHECK_EQUAL(p.nonce_window, 100U);
    BOOST_CHECK_EQUAL(p.max_delay_slots, 1024U);
    BOOST_CHECK(!p.futures_enabled);
    BOOST_CHECK(p.signer_mode == SignerMode::STRICT_SIGNING);
    BOOST_CHECK_EQUAL(p.pool_capacity, 4096U);
    BOOST_CHECK_EQUAL(p.outcome_index_slots, 1024U);
    BOOST_CHECK(Validate(p).has_value());
}

BOOST_AUTO_TEST_CASE(non_regtest_chains_unsupported)
{
    for (const ChainType chain : {ChainType::MAIN, ChainType::TESTNET, ChainType::TESTNET4, ChainType::SIGNET}) {
        const auto profile{DevRegtestProfile(chain)};
        BOOST_REQUIRE(!profile.has_value());
        BOOST_CHECK_EQUAL(ToString(profile.error().code), "UNSUPPORTED_NETWORK");

        ProfileParams moved{Dev()};
        moved.chain = chain;
        CheckCode(Validate(moved), ErrorCode::UNSUPPORTED_NETWORK);
        // The network check comes first, even before an outline stub.
        moved.price_band_enabled = true;
        CheckCode(Validate(moved), ErrorCode::UNSUPPORTED_NETWORK);
    }
}

BOOST_AUTO_TEST_CASE(enum_registries_pinned)
{
    BOOST_CHECK_EQUAL(static_cast<int>(MatchingProfile::A2_PRORATA), 1);
    BOOST_CHECK_EQUAL(static_cast<int>(FeeIncidence::SELLER), 1);
    BOOST_CHECK_EQUAL(static_cast<int>(FeeIncidence::PER_SIDE), 2);
    BOOST_CHECK_EQUAL(static_cast<int>(SignerMode::STRICT_SIGNING), 1);
    BOOST_CHECK_EQUAL(static_cast<int>(SignerMode::FOLDED_SIGNING), 2);
    BOOST_CHECK_EQUAL(ToString(MatchingProfile::A2_PRORATA), "A2_PRORATA");
    BOOST_CHECK_EQUAL(ToString(FeeIncidence::SELLER), "SELLER");
    BOOST_CHECK_EQUAL(ToString(FeeIncidence::PER_SIDE), "PER_SIDE");
    BOOST_CHECK_EQUAL(ToString(SignerMode::STRICT_SIGNING), "STRICT");
    BOOST_CHECK_EQUAL(ToString(SignerMode::FOLDED_SIGNING), "FOLDED");
    BOOST_CHECK_EQUAL(ToString(static_cast<MatchingProfile>(0)), "UNKNOWN");
    BOOST_CHECK_EQUAL(ToString(static_cast<FeeIncidence>(3)), "UNKNOWN");
    BOOST_CHECK_EQUAL(ToString(static_cast<SignerMode>(0)), "UNKNOWN");
    BOOST_CHECK_EQUAL(FM_VERSION, 1U);
}

BOOST_AUTO_TEST_CASE(validate_refuses_structural_errors)
{
    const std::vector<std::pair<std::string, std::function<void(ProfileParams&)>>> broken{
        {"empty profile_id", [](ProfileParams& p) { p.profile_id.clear(); }},
        {"long profile_id", [](ProfileParams& p) { p.profile_id = std::string(65, 'x'); }},
        {"fm_version 0", [](ProfileParams& p) { p.fm_version = 0; }},
        {"fm_version 2", [](ProfileParams& p) { p.fm_version = 2; }},
        {"matching 0", [](ProfileParams& p) { p.matching = static_cast<MatchingProfile>(0); }},
        {"matching 2", [](ProfileParams& p) { p.matching = static_cast<MatchingProfile>(2); }},
        {"incidence 0", [](ProfileParams& p) { p.fee_incidence = static_cast<FeeIncidence>(0); }},
        {"incidence 3", [](ProfileParams& p) { p.fee_incidence = static_cast<FeeIncidence>(3); }},
        {"signer 0", [](ProfileParams& p) { p.signer_mode = static_cast<SignerMode>(0); }},
        {"signer 3", [](ProfileParams& p) { p.signer_mode = static_cast<SignerMode>(3); }},
        {"fee_ppm", [](ProfileParams& p) { p.fee_ppm = 1'000'001; }},
        {"seat share", [](ProfileParams& p) { p.seat_share_percent = 101; }},
        {"actions cap", [](ProfileParams& p) { p.max_actions_per_slot = 0; }},
        {"payload cap", [](ProfileParams& p) { p.max_action_payload_bytes = 0; }},
        {"fills per order", [](ProfileParams& p) { p.max_fills_per_order = 0; }},
        {"fills per slot", [](ProfileParams& p) { p.max_fills_per_slot = 0; }},
        {"changed leaves", [](ProfileParams& p) { p.max_changed_leaves_per_slot = 0; }},
        {"open orders", [](ProfileParams& p) { p.max_open_orders_per_account = 0; }},
        {"nonce window", [](ProfileParams& p) { p.nonce_window = 0; }},
        {"max delay 0", [](ProfileParams& p) { p.max_delay_slots = 0; }},
        {"max delay u16", [](ProfileParams& p) { p.max_delay_slots = 65'536; p.outcome_index_slots = 65'536; }},
        {"pool", [](ProfileParams& p) { p.pool_capacity = 0; }},
        {"outcome index 0", [](ProfileParams& p) { p.outcome_index_slots = 0; }},
        {"outcome index short", [](ProfileParams& p) { p.outcome_index_slots = p.max_delay_slots - 1; }},
        {"band 0", [](ProfileParams& p) { p.price_band_percent = 0; }},
        {"band 101", [](ProfileParams& p) { p.price_band_percent = 101; }},
        {"fills order above slot", [](ProfileParams& p) { p.max_fills_per_order = p.max_fills_per_slot + 1; }},
    };
    for (const auto& [label, mutate] : broken) {
        ProfileParams p{Dev()};
        mutate(p);
        const auto result{Validate(p)};
        BOOST_CHECK_MESSAGE(!result.has_value() && result.error().code == ErrorCode::INVALID_INPUT,
                            label << ": expected INVALID_INPUT");
    }
    // Boundaries that stay valid.
    ProfileParams edge{Dev()};
    edge.fee_ppm = 1'000'000;
    edge.seat_share_percent = 100;
    edge.price_band_percent = 100;
    edge.profile_id = std::string(64, 'x');
    edge.max_delay_slots = 65'535;
    edge.outcome_index_slots = 65'535;
    edge.max_fills_per_order = edge.max_fills_per_slot;
    BOOST_CHECK(Validate(edge).has_value());
}

BOOST_AUTO_TEST_CASE(outline_stubs_return_their_task_ids)
{
    // The stub_inventory cases cover the same four refusals one by one; this
    // case also pins the precedence: structural errors before stubs.
    ProfileParams band{Dev()};
    band.price_band_enabled = true;
    CheckCode(Validate(band), ErrorCode::NOT_IMPLEMENTED, "M3-ENG-03");
    band.pool_capacity = 0;
    CheckCode(Validate(band), ErrorCode::INVALID_INPUT);

    ProfileParams per_side{Dev()};
    per_side.fee_incidence = FeeIncidence::PER_SIDE;
    CheckCode(Validate(per_side), ErrorCode::NOT_IMPLEMENTED, "M3-ENG-08");

    ProfileParams futures{Dev()};
    futures.futures_enabled = true;
    CheckCode(Validate(futures), ErrorCode::NOT_IMPLEMENTED, "M9-FUT-01");

    ProfileParams folded{Dev()};
    folded.signer_mode = SignerMode::FOLDED_SIGNING;
    CheckCode(Validate(folded), ErrorCode::NOT_IMPLEMENTED, "M4-STO-05");
}

BOOST_AUTO_TEST_CASE(profile_hash_golden)
{
    // Golden computed independently with Python hashlib from the documented encoding.
    const ProfileParams p{Dev()};
    BOOST_CHECK_EQUAL(HexStr(EncodeProfile(p)),
                      "011e666d322d646578312d726567746573742d70726f766973696f6e616c2f31010000000772656774657374016400"
                      "000001500000000004000000100000000400000040000000200000e80300000032000000640000000004000000010010"
                      "000000040000");
    BOOST_CHECK_EQUAL(EncodeProfile(p).size(), 101U);
    BOOST_CHECK_EQUAL(Hex(ProfileHash(p)), "c101c802495f73f37177b3ad68d3edb71a7e5a252ebbfc8533cad699e1756281");
}

BOOST_AUTO_TEST_CASE(profile_hash_changes_with_every_field)
{
    const ProfileParams base{Dev()};
    const uint256 base_hash{ProfileHash(base)};
    std::set<uint256> seen{base_hash};
    for (const auto& [name, mutate] : FieldMutations()) {
        ProfileParams changed{base};
        mutate(changed);
        BOOST_CHECK_MESSAGE(!(changed == base), name << ": mutation did not change the profile");
        const uint256 hash{ProfileHash(changed)};
        BOOST_CHECK_MESSAGE(hash != base_hash, name << ": ProfileHash did not change");
        BOOST_CHECK_MESSAGE(seen.insert(hash).second, name << ": ProfileHash equals another mutation's");
    }
}

BOOST_AUTO_TEST_CASE(domain_hash_golden_and_sensitivity)
{
    const NetworkId network{TestNetwork()};
    BOOST_CHECK_EQUAL(Hex(network.modern_chain_domain), "4f9cfaa7394620a6c3a45dac67ecb79cee9af65b59dbcd3095205bb5bed63a7c");
    const uint256 domain{DomainHash(network, FM_VERSION)};
    BOOST_CHECK_EQUAL(Hex(domain), "3b4fbb2d6135b9444d98dad5b1ec53dcdce23b1d73e8d08e2e6fdc02e57d42f6");

    std::set<uint256> seen{domain};
    for (size_t i{0}; i < network.magic.size(); ++i) {
        NetworkId other{network};
        other.magic[i] ^= 0x01;
        BOOST_CHECK(seen.insert(DomainHash(other, FM_VERSION)).second);
    }
    BOOST_CHECK(seen.insert(DomainHash(network, FM_VERSION + 1)).second);
    for (size_t i : {size_t{0}, size_t{31}}) {
        NetworkId other{network};
        other.modern_chain_domain.data()[i] ^= 0x01;
        BOOST_CHECK(seen.insert(DomainHash(other, FM_VERSION)).second);
    }
    // The chain type is not an input: the magic and the domain identify the network.
    NetworkId main_typed{network};
    main_typed.chain = ChainType::MAIN;
    BOOST_CHECK(DomainHash(main_typed, FM_VERSION) == domain);
}

BOOST_AUTO_TEST_CASE(profile_json_matches_code)
{
    const std::filesystem::path path{std::filesystem::path{FM2_SOURCE_ROOT} / "src" / "flowmesh2" / "PROFILE_DEX1_REGTEST.json"};
    UniValue json;
    BOOST_REQUIRE_MESSAGE(json.read(ReadFile(path)), "PROFILE_DEX1_REGTEST.json is not valid JSON");
    BOOST_REQUIRE(json.isObject());

    const ProfileParams p{Dev()};
    BOOST_CHECK_EQUAL(json["profile_hash"].get_str(), Hex(ProfileHash(p)));
    BOOST_CHECK_EQUAL(json["profile_id"].get_str(), p.profile_id);
    BOOST_CHECK_EQUAL(json["encoding_version"].getInt<int>(), static_cast<int>(PROFILE_ENCODING_VERSION));
    BOOST_CHECK(!json["mainnet_valid"].get_bool());

    const UniValue& params{json["params"]};
    BOOST_REQUIRE(params.isObject());
    const std::set<std::string> expected_keys{
        "profile_id", "fm_version", "chain", "matching", "fee_ppm", "fee_incidence", "seat_share_percent",
        "max_actions_per_slot", "max_action_payload_bytes", "max_fills_per_order", "max_fills_per_slot",
        "max_changed_leaves_per_slot", "max_open_orders_per_account", "price_band_enabled", "price_band_percent",
        "nonce_window", "max_delay_slots", "futures_enabled", "signer_mode", "pool_capacity", "outcome_index_slots"};
    BOOST_CHECK_EQUAL(expected_keys.size(), LEGACY_PROFILE_FIELDS);
    const auto keys{params.getKeys()};
    BOOST_CHECK(std::set<std::string>(keys.begin(), keys.end()) == expected_keys);
    BOOST_CHECK_EQUAL(keys.size(), LEGACY_PROFILE_FIELDS);

    const auto u32{[&](const char* key) { return params[key].getInt<uint32_t>(); }};
    BOOST_CHECK_EQUAL(params["profile_id"].get_str(), p.profile_id);
    BOOST_CHECK_EQUAL(u32("fm_version"), p.fm_version);
    BOOST_CHECK_EQUAL(params["chain"].get_str(), ChainTypeToString(p.chain));
    BOOST_CHECK_EQUAL(params["matching"].get_str(), ToString(p.matching));
    BOOST_CHECK_EQUAL(u32("fee_ppm"), p.fee_ppm);
    BOOST_CHECK_EQUAL(params["fee_incidence"].get_str(), ToString(p.fee_incidence));
    BOOST_CHECK_EQUAL(u32("seat_share_percent"), p.seat_share_percent);
    BOOST_CHECK_EQUAL(u32("max_actions_per_slot"), p.max_actions_per_slot);
    BOOST_CHECK_EQUAL(u32("max_action_payload_bytes"), p.max_action_payload_bytes);
    BOOST_CHECK_EQUAL(u32("max_fills_per_order"), p.max_fills_per_order);
    BOOST_CHECK_EQUAL(u32("max_fills_per_slot"), p.max_fills_per_slot);
    BOOST_CHECK_EQUAL(u32("max_changed_leaves_per_slot"), p.max_changed_leaves_per_slot);
    BOOST_CHECK_EQUAL(u32("max_open_orders_per_account"), p.max_open_orders_per_account);
    BOOST_CHECK_EQUAL(params["price_band_enabled"].get_bool(), p.price_band_enabled);
    BOOST_CHECK_EQUAL(u32("price_band_percent"), p.price_band_percent);
    BOOST_CHECK_EQUAL(u32("nonce_window"), p.nonce_window);
    BOOST_CHECK_EQUAL(u32("max_delay_slots"), p.max_delay_slots);
    BOOST_CHECK_EQUAL(params["futures_enabled"].get_bool(), p.futures_enabled);
    BOOST_CHECK_EQUAL(params["signer_mode"].get_str(), ToString(p.signer_mode));
    BOOST_CHECK_EQUAL(u32("pool_capacity"), p.pool_capacity);
    BOOST_CHECK_EQUAL(u32("outcome_index_slots"), p.outcome_index_slots);

    // The refusal table names exactly the four Validate stubs.
    const UniValue& refused{json["refused_until_implemented"]};
    BOOST_REQUIRE(refused.isObject());
    BOOST_CHECK_EQUAL(refused["price_band_enabled=true"].get_str(), "M3-ENG-03");
    BOOST_CHECK_EQUAL(refused["fee_incidence=PER_SIDE"].get_str(), "M3-ENG-08");
    BOOST_CHECK_EQUAL(refused["futures_enabled=true"].get_str(), "M9-FUT-01");
    BOOST_CHECK_EQUAL(refused["signer_mode=FOLDED"].get_str(), "M4-STO-05");
    BOOST_CHECK_EQUAL(refused.size(), 4U);
}

BOOST_AUTO_TEST_CASE(stable_q_profile_is_versioned_and_regtest_only)
{
    const auto built{StableQRegtestProfile(ChainType::REGTEST)};
    BOOST_REQUIRE(built.has_value());
    const ProfileParams profile{*built};
    BOOST_CHECK(profile.stable_fee_asset == std::optional<AssetId>{RegtestQuoteAsset()});
    BOOST_CHECK_EQUAL(profile.profile_id, STABLE_Q_PROFILE_ID);
    BOOST_CHECK_EQUAL(EncodeProfile(profile).front(), STABLE_Q_PROFILE_ENCODING_VERSION);
    BOOST_CHECK_EQUAL(Hex(ProfileHash(profile)), "5cc396e7254c54c792e2dfa33666268fb6cd9f73b41e6bdca648f054f3ae94ad");
    BOOST_CHECK(ProfileHash(profile) != ProfileHash(Dev()));
    BOOST_CHECK(Validate(profile).has_value());
    ProfileParams expected{Dev()};
    expected.profile_id = STABLE_Q_PROFILE_ID;
    expected.stable_fee_asset = RegtestQuoteAsset();
    expected.withdrawals_enabled = true;
    expected.withdrawal_fee_atoms = 1'000;
    BOOST_CHECK(profile == expected); // all other economics remain provisional test parameters
    for (const ChainType chain : {ChainType::MAIN, ChainType::TESTNET, ChainType::TESTNET4, ChainType::SIGNET}) {
        const auto other{StableQRegtestProfile(chain)};
        BOOST_REQUIRE(!other.has_value());
        BOOST_CHECK(other.error().code == ErrorCode::UNSUPPORTED_NETWORK);
        ProfileParams moved{profile};
        moved.chain = chain;
        CheckCode(Validate(moved), ErrorCode::UNSUPPORTED_NETWORK);
    }
    ProfileParams missing{profile};
    missing.stable_fee_asset.reset();
    CheckCode(Validate(missing), ErrorCode::INVALID_INPUT);
    ProfileParams native{profile};
    native.stable_fee_asset = AssetId::Native();
    CheckCode(Validate(native), ErrorCode::INVALID_INPUT);
    BOOST_CHECK(ProfileHash(native) != ProfileHash(profile));
    ProfileParams wrong{profile};
    wrong.stable_fee_asset = AssetId{uint256::ONE};
    CheckCode(Validate(wrong), ErrorCode::INVALID_INPUT);
    BOOST_CHECK(ProfileHash(wrong) != ProfileHash(profile));
    ProfileParams confused{Dev()};
    confused.stable_fee_asset = RegtestQuoteAsset();
    CheckCode(Validate(confused), ErrorCode::INVALID_INPUT);
    ProfileParams disabled{profile};
    disabled.withdrawals_enabled = false;
    CheckCode(Validate(disabled), ErrorCode::INVALID_INPUT);
    disabled.withdrawal_fee_atoms = 0;
    BOOST_CHECK(Validate(disabled).has_value());
    BOOST_CHECK(ProfileHash(disabled) != ProfileHash(profile));
    ProfileParams negative{profile};
    negative.withdrawal_fee_atoms = -1;
    CheckCode(Validate(negative), ErrorCode::INVALID_INPUT);
    ProfileParams too_large{profile};
    too_large.withdrawal_fee_atoms = MAX_ATOMS + 1;
    CheckCode(Validate(too_large), ErrorCode::INVALID_INPUT);
    ProfileParams changed_fee{profile};
    ++changed_fee.withdrawal_fee_atoms;
    BOOST_CHECK(ProfileHash(changed_fee) != ProfileHash(profile));
}

BOOST_AUTO_TEST_CASE(stable_q_profile_json_matches_code)
{
    const auto path{std::filesystem::path{FM2_SOURCE_ROOT} / "src" / "flowmesh2" / "PROFILE_STABLE_Q_REGTEST.json"};
    UniValue json;
    BOOST_REQUIRE(json.read(ReadFile(path)));
    const ProfileParams profile{StableQRegtestProfile(ChainType::REGTEST).value()};
    BOOST_CHECK_EQUAL(json["profile_id"].get_str(), profile.profile_id);
    BOOST_CHECK_EQUAL(json["encoding_version"].getInt<int>(), STABLE_Q_PROFILE_ENCODING_VERSION);
    BOOST_CHECK_EQUAL(json["stable_fee_asset"].get_str(), HexStr(RegtestQuoteAsset().id));
    BOOST_CHECK_EQUAL(json["encoded_profile"].get_str(), HexStr(EncodeProfile(profile)));
    BOOST_CHECK_EQUAL(json["profile_hash"].get_str(), Hex(ProfileHash(profile)));
    BOOST_CHECK(!json["mainnet_valid"].get_bool());
    BOOST_CHECK_EQUAL(json["provisional_fee_ppm"].getInt<uint32_t>(), profile.fee_ppm);
    BOOST_CHECK_EQUAL(json["provisional_seat_share_percent"].getInt<uint32_t>(), profile.seat_share_percent);
    BOOST_CHECK_EQUAL(json["withdrawals_enabled"].get_bool(), profile.withdrawals_enabled);
    BOOST_CHECK_EQUAL(json["withdrawal_fee_atoms"].getInt<Atoms>(), profile.withdrawal_fee_atoms);
}


BOOST_AUTO_TEST_CASE(stable_q_view_profile_preserves_archives_and_binds_explicit_policy)
{
    const auto legacy{StableQRegtestProfile(ChainType::REGTEST).value()};
    const auto current{StableQViewRegtestProfile(ChainType::REGTEST).value()};
    BOOST_CHECK(!legacy.view_bound_commits);
    BOOST_CHECK_EQUAL(Hex(ProfileHash(legacy)), "5cc396e7254c54c792e2dfa33666268fb6cd9f73b41e6bdca648f054f3ae94ad");
    BOOST_CHECK_EQUAL(EncodeProfile(legacy).front(), 2U);
    BOOST_CHECK(current.view_bound_commits);
    BOOST_CHECK_EQUAL(current.profile_id, STABLE_Q_VIEW_PROFILE_ID);
    BOOST_CHECK_EQUAL(EncodeProfile(current).front(), 3U);
    BOOST_CHECK_EQUAL(EncodeProfile(current).back(), 1U);
    BOOST_CHECK_EQUAL(Hex(ProfileHash(current)), "c1e76cd5d1d4b8e1a2c510ea964c9d2ff73a954370d6cb0c0985b1357cd930fc");
    BOOST_CHECK(Validate(current).has_value());
    auto expected{legacy}; expected.profile_id = STABLE_Q_VIEW_PROFILE_ID; expected.view_bound_commits = true;
    BOOST_CHECK(current == expected); // economics are unchanged; protocol/profile identity is distinct
    auto confused{legacy}; confused.view_bound_commits = true;
    CheckCode(Validate(confused), ErrorCode::INVALID_INPUT);
    confused = current; confused.view_bound_commits = false;
    CheckCode(Validate(confused), ErrorCode::INVALID_INPUT);
    confused = Dev(); confused.view_bound_commits = true;
    CheckCode(Validate(confused), ErrorCode::INVALID_INPUT);
    for (const auto chain : {ChainType::MAIN, ChainType::TESTNET, ChainType::TESTNET4, ChainType::SIGNET}) {
        const auto other{StableQViewRegtestProfile(chain)};
        BOOST_REQUIRE(!other.has_value());
        BOOST_CHECK(other.error().code == ErrorCode::UNSUPPORTED_NETWORK);
    }
}

BOOST_AUTO_TEST_CASE(stable_q_view_profile_json_matches_code)
{
    const auto path{std::filesystem::path{FM2_SOURCE_ROOT} / "src" / "flowmesh2" / "PROFILE_STABLE_Q_VIEW_REGTEST.json"};
    UniValue json;
    BOOST_REQUIRE(json.read(ReadFile(path)));
    const auto profile{StableQViewRegtestProfile(ChainType::REGTEST).value()};
    BOOST_CHECK_EQUAL(json["profile_id"].get_str(), profile.profile_id);
    BOOST_CHECK_EQUAL(json["encoding_version"].getInt<int>(), STABLE_Q_VIEW_PROFILE_ENCODING_VERSION);
    BOOST_CHECK_EQUAL(json["profile_hash"].get_str(), Hex(ProfileHash(profile)));
    BOOST_CHECK_EQUAL(json["encoded_profile"].get_str(), HexStr(EncodeProfile(profile)));
    BOOST_CHECK(json["view_bound_commits"].get_bool());
    BOOST_CHECK(!json["mainnet_valid"].get_bool());
    BOOST_CHECK_EQUAL(json["withdrawal_fee_atoms"].getInt<Atoms>(), profile.withdrawal_fee_atoms);
}

BOOST_AUTO_TEST_CASE(stable_q_network_profile_binds_source_policy_and_preserves_archives)
{
    const auto legacy{StableQRegtestProfile(ChainType::REGTEST).value()};
    const auto view{StableQViewRegtestProfile(ChainType::REGTEST).value()};
    const auto profile{StableQNetworkRegtestProfile(ChainType::REGTEST).value()};
    BOOST_CHECK_EQUAL(Hex(ProfileHash(legacy)), "5cc396e7254c54c792e2dfa33666268fb6cd9f73b41e6bdca648f054f3ae94ad");
    BOOST_CHECK_EQUAL(Hex(ProfileHash(view)), "c1e76cd5d1d4b8e1a2c510ea964c9d2ff73a954370d6cb0c0985b1357cd930fc");
    BOOST_CHECK_EQUAL(profile.profile_id, STABLE_Q_NETWORK_PROFILE_ID);
    BOOST_CHECK_EQUAL(EncodeProfile(profile).front(), 4U);
    BOOST_CHECK(profile.view_bound_commits && profile.source_range_commitment);
    BOOST_CHECK_EQUAL(profile.source_chain_id, 31337U);
    BOOST_CHECK_EQUAL(profile.source_confirmations, 2U);
    BOOST_CHECK(Validate(profile).has_value());
    for (const auto& mutate : std::vector<std::function<void(ProfileParams&)>>{
             [](auto& p) { p.source_range_commitment = false; }, [](auto& p) { p.source_chain_id = 1; },
             [](auto& p) { p.source_confirmations = 1; }, [](auto& p) { p.view_bound_commits = false; }}) {
        auto wrong{profile}; mutate(wrong);
        CheckCode(Validate(wrong), ErrorCode::INVALID_INPUT);
        BOOST_CHECK(ProfileHash(wrong) != ProfileHash(profile));
    }
    for (auto wrong : {legacy, view, Dev()}) {
        wrong.source_range_commitment = true;
        CheckCode(Validate(wrong), ErrorCode::INVALID_INPUT);
    }
    for (const auto chain : {ChainType::MAIN, ChainType::TESTNET, ChainType::TESTNET4, ChainType::SIGNET}) {
        const auto refused{StableQNetworkRegtestProfile(chain)};
        BOOST_REQUIRE(!refused.has_value());
        BOOST_CHECK(refused.error().code == ErrorCode::UNSUPPORTED_NETWORK);
    }
}

BOOST_AUTO_TEST_CASE(stable_q_network_profile_json_matches_code)
{
    const auto path{std::filesystem::path{FM2_SOURCE_ROOT} / "src" / "flowmesh2" / "PROFILE_STABLE_Q_NETWORK_REGTEST.json"};
    UniValue json;
    BOOST_REQUIRE(json.read(ReadFile(path)));
    const auto profile{StableQNetworkRegtestProfile(ChainType::REGTEST).value()};
    BOOST_CHECK_EQUAL(json["profile_id"].get_str(), profile.profile_id);
    BOOST_CHECK_EQUAL(json["encoding_version"].getInt<int>(), STABLE_Q_NETWORK_PROFILE_ENCODING_VERSION);
    BOOST_CHECK_EQUAL(json["profile_hash"].get_str(), Hex(ProfileHash(profile)));
    BOOST_CHECK_EQUAL(json["encoded_profile"].get_str(), HexStr(EncodeProfile(profile)));
    BOOST_CHECK(json["view_bound_commits"].get_bool());
    BOOST_CHECK(json["source_range_commitment"].get_bool());
    BOOST_CHECK_EQUAL(json["source_chain_id"].getInt<uint64_t>(), profile.source_chain_id);
    BOOST_CHECK_EQUAL(json["source_confirmations"].getInt<uint32_t>(), profile.source_confirmations);
    BOOST_CHECK(!json["mainnet_valid"].get_bool());
}

BOOST_AUTO_TEST_SUITE_END()
