// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license.
#include <boost/test/unit_test.hpp>
#include <bip324.h>
#include <key.h>
#include <array>
#include <span>
#include <vector>

BOOST_AUTO_TEST_SUITE(bip324_overlay_context_tests)
BOOST_AUTO_TEST_CASE(explicit_context_matches_cipher_vectors_across_rekey_without_global_params)
{
    std::array<unsigned char, 32> seed{};
    seed[31] = 1;
    CKey key1; key1.Set(seed.begin(), seed.end(), true);
    seed[31] = 2;
    CKey key2; key2.Set(seed.begin(), seed.end(), true);
    const std::array<std::byte, 32> entropy1{}, entropy2{std::byte{7}};
    BIP324Cipher sender{key1, entropy1}, receiver{key2, entropy2};
    BIP324Cipher reference{key1, entropy1}, reference_receiver{key2, entropy2};
    const MessageStartChars context{0xfa, 0xbf, 0xb5, 0xda};
    sender.InitializeForNetwork(receiver.GetOurPubKey(), true, context);
    receiver.InitializeForNetwork(sender.GetOurPubKey(), false, context);
    reference.InitializeForTesting(reference_receiver.GetOurPubKey(), true, context);
    reference_receiver.InitializeForTesting(reference.GetOurPubKey(), false, context);
    BOOST_CHECK(std::equal(sender.GetSessionID().begin(), sender.GetSessionID().end(), receiver.GetSessionID().begin()));
    BOOST_CHECK(std::equal(sender.GetSessionID().begin(), sender.GetSessionID().end(), reference.GetSessionID().begin()));
    for (unsigned i{0}; i < BIP324Cipher::REKEY_INTERVAL + 2; ++i) {
        std::vector<std::byte> message(1 + i % 37, std::byte{static_cast<unsigned char>(i)});
        std::vector<std::byte> encrypted(message.size() + BIP324Cipher::EXPANSION), expected(encrypted.size());
        sender.Encrypt(message, {}, false, encrypted);
        reference.Encrypt(message, {}, false, expected);
        BOOST_CHECK(encrypted == expected);
        const auto length{receiver.DecryptLength(std::span{encrypted}.first(BIP324Cipher::LENGTH_LEN))};
        BOOST_REQUIRE_EQUAL(length, message.size());
        std::vector<std::byte> decoded(length);
        bool ignored{true};
        BOOST_REQUIRE(receiver.Decrypt(std::span{encrypted}.subspan(BIP324Cipher::LENGTH_LEN), {}, ignored, decoded));
        BOOST_CHECK(!ignored && decoded == message);
    }
}
BOOST_AUTO_TEST_SUITE_END()
