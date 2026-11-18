// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/chain/chain_adapter.h>
#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/body.h>
#include <flowmesh2/core/hashes.h>
#include <flowmesh2/dev/dev_chain_file.h>
#include <flowmesh2/netio/genesis_doc.h>
#include <flowmesh2/netio/mesh_handshake.h>
#include <flowmesh2/test/util/agreement_fixtures.h>
#include <flowmesh2/test/util/keys.h>
#include <flowmesh2/test/util/ledger_fixtures.h>
#include <flowmesh2/test/util/require.h>
#include <flowmesh2/test/util/storage_fixtures.h>
#include <crypto/bls.h>
#include <key.h>
#include <pubkey.h>
#include <util/fs.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <fstream>
#include <future>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

/** The genesis document and the dev chain file (M5-NODE-01, O-105): the
 * document round-trips, every party computes the same seat set and genesis
 * head from it, malformed documents are refused; the dev chain file appends
 * and tails blocks, waits on a partial record and refuses corruption.
 */
using namespace flowmesh2;
using namespace flowmesh2::netio;
using namespace flowmesh2::test;

namespace {

GenesisDoc TestGenesis()
{
    GenesisDoc doc;
    doc.network.chain = ChainType::REGTEST;
    doc.network.magic = {0xf2, 0x51, 0x4d, 0x10};
    doc.network.modern_chain_domain = LabelHash("fm2/test/genesis/domain");
    doc.mesh_magic = {0xf2, 0x51, 0x4d, 0x11};
    doc.base = Anchor{129, LabelHash("fm2/test/genesis/block/129")};
    doc.facts_start = FactCursor{130, 0};
    doc.assets.emplace("B3", AssetId::Native());
    const std::string rusd{"rUSD"};
    doc.assets.emplace(rusd, AssetId{Tagged(HashTag::TEST_ASSET, std::span{reinterpret_cast<const unsigned char*>(rusd.data()), rusd.size()})});
    doc.markets.push_back(GenesisMarket{"B3", "rUSD", 1'000'000, 1});
    const std::vector<std::string> names{"s0", "s1", "s2", "s3"};
    for (uint32_t i{0}; i < 4; ++i) {
        const bls::SecretKey key{BlsKeyFromLabel(SeatLabel(names[i]))};
        GenesisSeat seat;
        seat.index = i;
        seat.bls_pubkey = key.GetPublicKey().Compressed();
        seat.pop = key.SignPoP().Compressed();
        seat.mesh_port = static_cast<uint16_t>(20'000 + i);
        seat.api_port = static_cast<uint16_t>(21'000 + i);
        seat.endpoint.seat = i;
        seat.endpoint.addresses = {"127.0.0.1"};
        seat.endpoint.port = seat.mesh_port;
        seat.endpoint.counter = 1;
        CKey transport;
        const uint256 secret{LabelHash("fm2/test/genesis/transport/" + names[i])};
        transport.Set(secret.begin(), secret.end(), true);
        const XOnlyPubKey xonly{transport.GetPubKey()};
        std::copy(xonly.begin(), xonly.end(), seat.endpoint.transport_key.begin());
        const uint256 digest{EndpointDigest(seat.endpoint, doc.Domain())};
        seat.endpoint.signature = key.Sign(std::span<const unsigned char>{digest.data(), digest.size()}).Compressed();
        doc.seats.push_back(seat);
    }
    return doc;
}

} // namespace

BOOST_AUTO_TEST_SUITE(genesis_doc_tests)

BOOST_AUTO_TEST_CASE(the_document_round_trips_and_every_party_computes_the_same_genesis)
{
    const GenesisDoc doc{TestGenesis()};
    const std::string json{doc.Json()};
    BOOST_CHECK(json.find("\"format\": \"fm2-genesis/1\"") != std::string::npos);
    const Result<GenesisDoc> back{GenesisDoc::Parse(json)};
    RequireOk(back, "GenesisDoc::Parse");
    BOOST_CHECK_EQUAL(back->Json(), json);
    BOOST_CHECK(back->network.magic == doc.network.magic && back->network.modern_chain_domain == doc.network.modern_chain_domain);
    BOOST_CHECK(back->mesh_magic == doc.mesh_magic && back->base == doc.base && back->facts_start == doc.facts_start);
    BOOST_REQUIRE_EQUAL(back->seats.size(), doc.seats.size());
    for (size_t i{0}; i < doc.seats.size(); ++i) BOOST_CHECK(back->seats[i] == doc.seats[i]);
    BOOST_CHECK(back->assets == doc.assets && back->markets == doc.markets);
    // The seat set is the test seats' and every endpoint record verifies against it.
    const Result<SeatSet> seats{back->SeatSetOf()};
    RequireOk(seats, "SeatSetOf");
    BOOST_CHECK(seats->Root() == TestSeats({"s0", "s1", "s2", "s3"}).Root());
    for (const net::SeatEndpointRecord& record : back->Endpoints()) RequireOk(VerifyEndpointRecord(record, *seats, back->Domain()), "record");
    // The genesis head is the same from two parses, and the markets resolve.
    const ProfileParams profile{RegtestProfile()};
    const Result<state::HeadView> head{back->GenesisHead(profile, *seats)};
    RequireOk(head, "GenesisHead");
    const Result<state::HeadView> again{doc.GenesisHead(profile, *seats)};
    RequireOk(again, "GenesisHead again");
    BOOST_CHECK(head->HeaderHash() == again->HeaderHash());
    BOOST_CHECK_EQUAL(head->Header().height, 0U);
    BOOST_CHECK(head->Header().anchor == doc.base);
    const Result<MarketDescriptor> m1{back->Market("B3", "rUSD")};
    RequireOk(m1, "Market");
    BOOST_CHECK(m1->base == AssetId::Native());
    BOOST_CHECK_EQUAL(m1->base_atoms_per_lot, 1'000'000U);
    BOOST_CHECK(!back->Market("TOK", "rUSD").has_value());
    BOOST_CHECK(back->Asset("rUSD").has_value() && !back->Asset("nope").has_value());
    BOOST_CHECK(back->Authority(profile, *seats) == (AuthorityVersion{0, seats->Root(), ProfileHash(profile)}));
}

BOOST_AUTO_TEST_CASE(malformed_documents_are_refused)
{
    const GenesisDoc doc{TestGenesis()};
    RequireRefusal(GenesisDoc::Parse("not json"), ErrorCode::DECODE, GENESIS_JSON, "not json");
    RequireRefusal(GenesisDoc::Parse("{\"format\":\"fm2-genesis/9\"}"), ErrorCode::DECODE, GENESIS_FORMAT_UNKNOWN, "format");
    std::string mainnet{doc.Json()};
    mainnet.replace(mainnet.find("\"regtest\""), 9, "\"main\"");
    RequireRefusal(GenesisDoc::Parse(mainnet), ErrorCode::UNSUPPORTED_NETWORK, GENESIS_NOT_REGTEST, "mainnet");
    GenesisDoc unordered{doc};
    std::swap(unordered.seats[0], unordered.seats[1]);
    RequireRefusal(GenesisDoc::Parse(unordered.Json()), ErrorCode::DECODE, GENESIS_SEATS, "seats out of order");
    GenesisDoc orphan{doc};
    orphan.markets.push_back(GenesisMarket{"B3", "nope", 1, 1});
    RequireRefusal(GenesisDoc::Parse(orphan.Json()), ErrorCode::DECODE, GENESIS_ASSET_UNKNOWN, "unknown asset");
    GenesisDoc forged{doc};
    forged.seats[2].endpoint.signature[0] ^= 0x01;
    const Result<GenesisDoc> parsed{GenesisDoc::Parse(forged.Json())};
    RequireOk(parsed, "a forged record parses");
    RequireRefusal(VerifyEndpointRecord(parsed->seats[2].endpoint, parsed->SeatSetOf().value(), parsed->Domain()), ErrorCode::VERIFY_FAILED,
                   net::TCP_MESH_ENDPOINT_SIGNATURE, "but does not verify");
    GenesisDoc bad_pop{doc};
    bad_pop.seats[1].pop[5] ^= 0x01;
    BOOST_CHECK(!GenesisDoc::Parse(bad_pop.Json()).value().SeatSetOf().has_value());
    // Numbers that are not unsigned integers are decode errors, not crashes (review 2026-11-16, R3): the base height
    // negative, fractional, or above 2^64.
    const std::string json{doc.Json()};
    const std::string height{"\"height\": " + std::to_string(doc.base.height)}; // UniValue pretty-prints with a space
    BOOST_REQUIRE(json.find(height) != std::string::npos);
    for (const char* bad : {"\"height\": -1", "\"height\": 1.5", "\"height\": 99999999999999999999", "\"height\": 1e3"}) {
        std::string mutated{json};
        mutated.replace(mutated.find(height), height.size(), bad);
        const Result<GenesisDoc> parsed{GenesisDoc::Parse(mutated)};
        BOOST_CHECK_MESSAGE(!parsed.has_value() && parsed.error().code == ErrorCode::DECODE, std::string{"accepted "} + bad);
    }
}

BOOST_AUTO_TEST_CASE(the_dev_chain_file_appends_tails_and_refuses_corruption)
{
    const fs::path dir{fs::PathFromString(FM2_TEST_TMP_DIR) / fs::u8path("devchain_tests")};
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path file{dir / fs::u8path("devchain.log")};
    BOOST_CHECK(dev::ReadDevChain(file).value().empty()); // an absent file is an empty chain
    dev::DevChainTail tail{file};
    BOOST_CHECK(tail.Poll().value().empty());
    DepositFact deposit;
    deposit.cursor = FactCursor{130, 0};
    deposit.outpoint = dev::DevChainOutpoint(130, 0);
    deposit.account = AccountId{LabelHash("fm2/test/devchain/alice")};
    deposit.asset = AssetId::Native();
    deposit.amount = 5;
    const dev::DevChainBlock first{130, dev::DevChainBlockHash(130), {Fact{deposit}}};
    const dev::DevChainBlock second{131, dev::DevChainBlockHash(131), {}};
    RequireOk(dev::AppendDevChainBlock(file, first), "append 130");
    RequireRefusal(dev::AppendDevChainBlock(file, dev::DevChainBlock{133, dev::DevChainBlockHash(133), {}}), ErrorCode::INVALID_INPUT,
                   dev::DEVCHAIN_HEIGHT, "a gap");
    // A deposit above MAX_ATOMS does not encode: it is refused and nothing is written, where it used to become an undecodable
    // block that wedged every seat (review 2026-11-16, R8).
    DepositFact oversized{deposit};
    oversized.cursor = FactCursor{131, 0};
    oversized.amount = MAX_ATOMS + 1;
    RequireCode(dev::AppendDevChainBlock(file, dev::DevChainBlock{131, dev::DevChainBlockHash(131), {Fact{oversized}}}), ErrorCode::INVALID_INPUT,
                "a deposit outside the atom range");
    BOOST_CHECK_EQUAL(dev::ReadDevChain(file).value().size(), 1U);
    RequireOk(dev::AppendDevChainBlock(file, second), "append 131");
    const Result<std::vector<dev::DevChainBlock>> all{dev::ReadDevChain(file)};
    RequireOk(all, "ReadDevChain");
    BOOST_REQUIRE_EQUAL(all->size(), 2U);
    BOOST_CHECK(all->at(0) == first);
    BOOST_CHECK(all->at(1) == second);
    const Result<std::vector<dev::DevChainBlock>> polled{tail.Poll()};
    RequireOk(polled, "Poll");
    BOOST_CHECK_EQUAL(polled->size(), 2U);
    BOOST_CHECK(tail.Poll().value().empty());
    // A partial record at the tail waits; completed, it arrives once.
    const std::vector<unsigned char> payload{dev::EncodeDevChainBlock(dev::DevChainBlock{132, dev::DevChainBlockHash(132), {}}).value()};
    {
        std::ofstream out{fs::PathToString(file), std::ios::binary | std::ios::app};
        const uint32_t len{static_cast<uint32_t>(payload.size())};
        out.write(reinterpret_cast<const char*>(&len), 4); // little-endian host
    }
    BOOST_CHECK(tail.Poll().value().empty());
    // Corruption (a wrong CRC) is fatal for a reader.
    {
        std::ofstream out{fs::PathToString(file), std::ios::binary | std::ios::app};
        const uint32_t crc{0xdeadbeef};
        out.write(reinterpret_cast<const char*>(&crc), 4);
        out.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    }
    RequireRefusal(tail.Poll(), ErrorCode::STORAGE_FATAL, dev::DEVCHAIN_CORRUPT, "bad crc");
    RequireRefusal(dev::ReadDevChain(file), ErrorCode::STORAGE_FATAL, dev::DEVCHAIN_CORRUPT, "bad crc everywhere");
    fs::remove_all(dir);
}

BOOST_AUTO_TEST_CASE(the_dev_chain_file_takes_one_writer_and_cuts_a_torn_tail)
{
    // Review workflow of 2026-11-18: AppendDevChainBlock read the file, checked the height and appended with no lock, after
    // whatever the file ended with. Two faucet calls at once could both append the same next height. After a writer that
    // died mid-record, the next block went in behind the torn bytes: every reader then took the torn length over the new
    // record, failed its CRC, and every seat tailing the file stopped for good. An append now holds an exclusive lock on
    // the file from its read to its fsync, and cuts a torn tail it finds under the lock (only a dead writer leaves one).
    const fs::path dir{fs::PathFromString(FM2_TEST_TMP_DIR) / fs::u8path("devchain_writer_tests")};
    fs::remove_all(dir);
    fs::create_directories(dir);
    const auto block{[](uint32_t height) { return dev::DevChainBlock{height, dev::DevChainBlockHash(height), {}}; }};
    // A writer that died four bytes into the record after block 130.
    const fs::path torn{dir / fs::u8path("torn.log")};
    RequireOk(dev::AppendDevChainBlock(torn, block(130)), "append 130");
    {
        const std::vector<unsigned char> payload{dev::EncodeDevChainBlock(block(131)).value()};
        std::ofstream out{fs::PathToString(torn), std::ios::binary | std::ios::app};
        const uint32_t len{static_cast<uint32_t>(payload.size())};
        out.write(reinterpret_cast<const char*>(&len), 4); // little-endian host
    }
    BOOST_CHECK_EQUAL(dev::ReadDevChain(torn).value().size(), 1U); // the torn record waits, as a partial one does
    RequireOk(dev::AppendDevChainBlock(torn, block(131)), "append after a torn tail");
    const Result<std::vector<dev::DevChainBlock>> after_torn{dev::ReadDevChain(torn)};
    BOOST_CHECK_MESSAGE(after_torn.has_value() && after_torn->size() == 2 && after_torn->back() == block(131),
                        "the chain after the torn tail: " << (after_torn.has_value() ? std::to_string(after_torn->size()) + " blocks"
                                                                                      : std::string{after_torn.error().detail}));
    // One writer at a time: while another holds the file's lock, an append waits for it.
    const fs::path shared{dir / fs::u8path("shared.log")};
    RequireOk(dev::AppendDevChainBlock(shared, block(130)), "append 130");
    const int held{::open(fs::PathToString(shared).c_str(), O_RDWR | O_CLOEXEC)};
    BOOST_REQUIRE(held >= 0);
    BOOST_REQUIRE_EQUAL(::flock(held, LOCK_EX), 0);
    std::future<Result<void>> appending{std::async(std::launch::async, [&] { return dev::AppendDevChainBlock(shared, block(131)); })};
    const uintmax_t one_block{fs::file_size(shared)};
    BOOST_CHECK(appending.wait_for(std::chrono::milliseconds{300}) == std::future_status::timeout);
    BOOST_CHECK_EQUAL(fs::file_size(shared), one_block); // nothing written (a read would wait for the lock too)
    ::close(held); // the lock goes with it
    RequireOk(appending.get(), "append once the lock is free");
    BOOST_CHECK_EQUAL(dev::ReadDevChain(shared).value().size(), 2U);
    // A writer that lost the race refuses its block instead of writing the height again.
    RequireRefusal(dev::AppendDevChainBlock(shared, block(131)), ErrorCode::INVALID_INPUT, dev::DEVCHAIN_HEIGHT, "131 again");
    fs::remove_all(dir);
}

BOOST_AUTO_TEST_CASE(a_dev_chain_reader_never_reads_across_a_writer)
{
    // Codex review of 2026-11-18 over 7d326476: readers took no lock, so a reader could read part of a torn record, then,
    // after an append cut the tail and wrote the next record in its place, read on into the new bytes. The record it put
    // together failed its CRC on a valid file: STORAGE_FATAL for an fm2d tailing it, or at its start. Readers now hold a
    // shared lock on the file for their read, so an append (exclusive) never runs during one. ReadDevChain waits for
    // the lock. A tail's Poll runs on fm2d's sealing thread every millisecond, so it does not wait: while a writer
    // holds the file it finds nothing new, and it takes the block on a later poll (advisor review of 2026-11-18).
    const fs::path dir{fs::PathFromString(FM2_TEST_TMP_DIR) / fs::u8path("devchain_reader_tests")};
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path file{dir / fs::u8path("devchain.log")};
    RequireOk(dev::AppendDevChainBlock(file, dev::DevChainBlock{130, dev::DevChainBlockHash(130), {}}), "append 130");
    dev::DevChainTail tail{file};
    const int held{::open(fs::PathToString(file).c_str(), O_RDWR | O_CLOEXEC)};
    BOOST_REQUIRE(held >= 0);
    BOOST_REQUIRE_EQUAL(::flock(held, LOCK_EX), 0); // what an append holds
    std::future<Result<std::vector<dev::DevChainBlock>>> reading{std::async(std::launch::async, [&] { return dev::ReadDevChain(file); })};
    std::future<Result<std::vector<dev::DevChainBlock>>> polling{std::async(std::launch::async, [&] { return tail.Poll(); })};
    BOOST_CHECK(reading.wait_for(std::chrono::milliseconds{300}) == std::future_status::timeout);
    const bool poll_returned{polling.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready};
    BOOST_CHECK_MESSAGE(poll_returned, "a poll waited for the writer's lock");
    ::close(held);
    BOOST_CHECK_EQUAL(reading.get().value().size(), 1U);
    const Result<std::vector<dev::DevChainBlock>> while_held{polling.get()};
    RequireOk(while_held, "Poll while the writer held the file");
    if (poll_returned) BOOST_CHECK(while_held->empty());
    const size_t polled_before{while_held->size()};
    BOOST_CHECK_EQUAL(polled_before + tail.Poll().value().size(), 1U); // the block arrives once
    fs::remove_all(dir);
}

BOOST_AUTO_TEST_CASE(the_faucet_check_refuses_a_block_no_seat_would_take)
{
    // Review workflow of 2026-11-18: R8 kept undecodable blocks out of the dev chain file, but the faucet still wrote
    // blocks every fm2d refuses (more than MAX_SLOT_FACTS deposits, a protocol account) or that no slot can carry (an
    // asset's deposits past MAX_ATOMS, O-34). A seat that tails such a file logs the refusal and never takes another
    // block, and fails on every restart. CheckDevChainBlock answers what a seat would.
    const GenesisDoc doc{TestGenesis()};
    const SeatSet seats{doc.SeatSetOf().value()};
    const AuthorityVersion authority{doc.Authority(RegtestProfile(), seats)};
    const auto deposit{[](uint32_t height, uint32_t index, const std::string& who, Atoms amount) {
        DepositFact fact;
        fact.cursor = FactCursor{height, index};
        fact.outpoint = dev::DevChainOutpoint(height, index);
        fact.account = AccountId{LabelHash("fm2/test/devchain/" + who)};
        fact.asset = AssetId::Native();
        fact.amount = amount;
        return Fact{fact};
    }};
    const auto check{[&](const std::vector<dev::DevChainBlock>& existing, const dev::DevChainBlock& block) {
        return dev::CheckDevChainBlock(doc.network, authority, seats, doc.base, existing, block);
    }};
    // A block of MAX_SLOT_FACTS deposits is taken; one more is not.
    dev::DevChainBlock full{130, dev::DevChainBlockHash(130), {}};
    for (uint32_t i{0}; i < MAX_SLOT_FACTS; ++i) full.facts.push_back(deposit(130, i, "many", 1));
    RequireOk(check({}, full), "MAX_SLOT_FACTS deposits");
    full.facts.push_back(deposit(130, static_cast<uint32_t>(MAX_SLOT_FACTS), "many", 1));
    RequireRefusal(check({}, full), ErrorCode::INVALID_INPUT, chain::BATCH_TOO_MANY, "one deposit too many");
    // An asset's deposits up to MAX_ATOMS over the file are taken; one atom more is not, in another block or the same one.
    const dev::DevChainBlock most{130, dev::DevChainBlockHash(130), {deposit(130, 0, "alice", MAX_ATOMS)}};
    RequireOk(check({}, most), "MAX_ATOMS");
    RequireRefusal(check({most}, dev::DevChainBlock{131, dev::DevChainBlockHash(131), {deposit(131, 0, "bob", 1)}}), ErrorCode::INVALID_INPUT,
                   dev::DEVCHAIN_DEPOSIT_TOTAL, "one atom past MAX_ATOMS in the next block");
    RequireRefusal(check({}, dev::DevChainBlock{130, dev::DevChainBlockHash(130), {deposit(130, 0, "alice", MAX_ATOMS - 1), deposit(130, 1, "bob", 2)}}),
                   ErrorCode::INVALID_INPUT, dev::DEVCHAIN_DEPOSIT_TOTAL, "one atom past MAX_ATOMS in one block");
    // A height the adapter does not expect next is refused too (its cursors name another block).
    RequireCode(check({most}, dev::DevChainBlock{132, dev::DevChainBlockHash(132), {deposit(132, 0, "bob", 1)}}), ErrorCode::INVALID_INPUT, "a gap");
}

BOOST_AUTO_TEST_SUITE_END()
