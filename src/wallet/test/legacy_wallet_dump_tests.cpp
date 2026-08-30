// Copyright (c) 2026 The B3 Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <wallet/legacy_wallet_dump.h>

#include <key_io.h>
#include <test/util/setup_common.h>
#include <util/result.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <sstream>
#include <string>

namespace wallet {
namespace {

CKey TestKey(unsigned char value, bool compressed)
{
    std::array<unsigned char, 32> secret{};
    secret.back() = value;
    CKey key;
    key.Set(secret.begin(), secret.end(), compressed);
    BOOST_REQUIRE(key.IsValid());
    return key;
}

std::string DumpRecord(const CKey& key, const std::string& timestamp, const std::string& role)
{
    return EncodeSecret(key) + " " + timestamp + " " + role + " # addr=" +
           EncodeDestination(PKHash{key.GetPubKey()});
}

util::Result<LegacyWalletDump> Parse(const std::string& text)
{
    std::istringstream stream{text};
    return ParseLegacyWalletDump(stream);
}

std::string Error(const util::Result<LegacyWalletDump>& result)
{
    return util::ErrorString(result).original;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(legacy_wallet_dump_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(valid_dump_and_exact_duplicate)
{
    const CKey compressed{TestKey(1, true)};
    const CKey uncompressed{TestKey(2, false)};
    const std::string first{DumpRecord(compressed, "2017-09-18T12:34:56Z", "label=Cold%20Storage%25")};
    const std::string second{DumpRecord(uncompressed, "2018-01-02T03:04:05Z", "change=1")};
    auto result{Parse("# Wallet dump created by B3-Coin v3\r\n# comment\r\n\r\n" + first + "\r\n" + second + "\r\n" + first + "\r\n# End of dump\r\n")};

    BOOST_REQUIRE(result);
    BOOST_CHECK_EQUAL(result->entries.size(), 2U);
    BOOST_CHECK_EQUAL(result->entries[0].label.value(), "Cold Storage%");
    BOOST_CHECK(result->entries[0].role == LegacyWalletDumpRole::LABEL);
    BOOST_CHECK(result->entries[1].role == LegacyWalletDumpRole::CHANGE);
    BOOST_CHECK(!result->entries[1].label.has_value());
    BOOST_CHECK(result->entries[0].key.IsCompressed());
    BOOST_CHECK(!result->entries[1].key.IsCompressed());
    BOOST_CHECK_EQUAL(result->earliest_timestamp, 1505738096);
}

BOOST_AUTO_TEST_CASE(rejects_malformed_without_echoing_secrets)
{
    const CKey key{TestKey(3, true)};
    const std::string wif{EncodeSecret(key)};
    const std::string good{DumpRecord(key, "2017-09-18T12:34:56Z", "reserve=1")};

    const std::array<std::string, 8> malformed{
        good + "\n# End of dump\n",
        "# Wallet dump created by B3-Coin v3\n" + good + "\n",
        "# Wallet dump created by B3-Coin v3\n# End of dump\n",
        "# Wallet dump created by B3-Coin v3\nnot-a-secret 2017-09-18T12:34:56Z reserve=1 # addr=invalid\n# End of dump\n",
        "# Wallet dump created by B3-Coin v3\n" + DumpRecord(key, "2017-09-18T99:34:56Z", "reserve=1") + "\n# End of dump\n",
        "# Wallet dump created by B3-Coin v3\n" + DumpRecord(key, "2017-09-18T12:34:56Z", "label=bad%2x") + "\n# End of dump\n",
        "# Wallet dump created by B3-Coin v3\n" + wif + " 2017-09-18T12:34:56Z reserve=1 # addr=ScmRU7h7sEmP9YDnFec6KzkAXUwrLkoVMV\n# End of dump\n",
        "# Wallet dump created by B3-Coin v3\n" + good + "\n" + DumpRecord(key, "2017-09-18T12:34:56Z", "change=1") + "\n# End of dump\n",
    };

    for (const auto& text : malformed) {
        auto result{Parse(text)};
        BOOST_REQUIRE(!result);
        BOOST_CHECK(Error(result).find(wif) == std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(rejects_data_after_end_marker)
{
    const CKey key{TestKey(4, true)};
    auto result{Parse("# Wallet dump created by B3-Coin v3\n# End of dump\n" +
                      DumpRecord(key, "2017-09-18T12:34:56Z", "label=") + "\n")};
    BOOST_REQUIRE(!result);
    BOOST_CHECK(Error(result).find("after end marker") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace wallet
