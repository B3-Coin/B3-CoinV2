// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/cli/https_client.h>
#include <flowmesh2/cli/outbox.h>
#include <flowmesh2/cli/reply_json.h>
#include <flowmesh2/cli/signer.h>
#include <flowmesh2/cli/trust.h>
#include <flowmesh2/core/action.h>
#include <flowmesh2/core/facts.h>
#include <flowmesh2/net/https_sse_api.h>
#include <flowmesh2/netio/api_json.h>
#include <flowmesh2/sim/sim_client.h>
#include <flowmesh2/sim/sim_node.h>
#include <flowmesh2/sim/trace.h>
#include <flowmesh2/storage/record.h>
#include <flowmesh2/test/util/ledger_fixtures.h>
#include <flowmesh2/test/util/require.h>
#include <flowmesh2/test/util/storage_fixtures.h>
#include <key.h>
#include <univalue.h>
#include <util/fs.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

/** The client's parts (M5-CLI-01, O-106): the outbox keeps exact bytes and
 * their results across a reopen and refuses what it must; the key signs an
 * action once and the account follows the key; the replies of the API
 * rebuild into the structs b3verify takes and verify against the node's
 * trust inputs, and a tampered envelope does not; the pinned TLS client
 * refuses another certificate and talks to the right one.
 */
using namespace flowmesh2;
using namespace flowmesh2::cli;
using namespace flowmesh2::sim;
using namespace flowmesh2::test;

namespace {

fs::path CliDir(const std::string& name)
{
    const fs::path dir{fs::PathFromString(FM2_TEST_TMP_DIR) / fs::u8path("cli_" + name)};
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

//! A simulated k = 1 node with two funded clients and, on request, the HTTPS API in front of it.
struct Rig {
    TraceRecorder trace;
    std::unique_ptr<SimNode> node;
    std::optional<SimClient> alice;
    std::optional<SimClient> bob;
    std::unique_ptr<net::HttpsSseApi> api;
    std::atomic<bool> pumping{false};
    std::thread pumper;
    fs::path dir;

    Rig()
    {
        Result<std::unique_ptr<SimNode>> created{SimNode::Create(SimNodeConfig{}, trace)};
        RequireOk(created, "SimNode::Create");
        node = std::move(created).value();
        const std::vector<MarketDescriptor> markets{node->Markets().begin(), node->Markets().end()};
        alice = SimClient::Create("alice", node->Profile(), node->Domain(), node->Seats(), markets).value();
        bob = SimClient::Create("bob", node->Profile(), node->Domain(), node->Seats(), markets).value();
        std::vector<Fact> facts;
        DepositFact a;
        a.cursor = FactCursor{SIM_BASE_HEIGHT + 1, 0};
        a.outpoint = Outpoint{SimLabelHash("fm2/test/cli/T1"), 0};
        a.account = alice->Account();
        a.asset = SimAsset("rUSD");
        a.amount = 1'000'000'000;
        DepositFact b;
        b.cursor = FactCursor{SIM_BASE_HEIGHT + 1, 1};
        b.outpoint = Outpoint{SimLabelHash("fm2/test/cli/T2"), 0};
        b.account = bob->Account();
        b.asset = AssetId::Native();
        b.amount = 10'000'000'000;
        facts.push_back(Fact{a});
        facts.push_back(Fact{b});
        RequireOk(node->Chain().AppendBlock(SimBlockHash(SIM_BASE_HEIGHT + 1), facts), "AppendBlock");
        RequireOk(node->SealSlot(), "SealSlot 1");
    }
    Trust TrustOf() const
    {
        return Trust{node->Profile(), node->Domain(), node->Seats(), std::vector<MarketDescriptor>{node->Markets().begin(), node->Markets().end()}};
    }
    void Serve()
    {
        dir = CliDir("https_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        net::HttpsSseConfig config;
        config.profile = node->Profile();
        config.dev_self_signed = true;
        config.self_signed_dir = fs::PathToString(dir);
        config.heartbeat_ms = 150;
        Result<std::unique_ptr<net::HttpsSseApi>> served{net::HttpsSseApi::Create(config, node->Api())};
        RequireOk(served, "HttpsSseApi::Create");
        api = std::move(served).value();
        pumping = true;
        pumper = std::thread{[this] {
            while (pumping) {
                (void)node->Pump(16);
                api->Notify();
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
            }
        }};
    }
    ~Rig()
    {
        pumping = false;
        if (pumper.joinable()) pumper.join();
        if (api) api->Stop();
        if (!dir.empty()) fs::remove_all(dir);
    }
};

std::string Text(const UniValue& value) { return value.write(); }

} // namespace

BOOST_AUTO_TEST_SUITE(cli_tests)

BOOST_AUTO_TEST_CASE(the_outbox_keeps_exact_bytes_and_results_across_a_reopen)
{
    Rig rig;
    const fs::path dir{CliDir("outbox")};
    const ProfileParams& profile{rig.node->Profile()};
    const MarketDescriptor market{rig.node->Markets().front()};
    const std::vector<unsigned char> bid{rig.alice->Place("bid", 1, market, Side::BUY, TimeInForce::GTC, 100, 5, 1).value()};
    const std::vector<unsigned char> ask{rig.bob->Place("ask", 1, market, Side::SELL, TimeInForce::GTC, 100, 5, 1).value()};
    const std::vector<unsigned char> bid2{rig.alice->Place("bid2", 7, market, Side::BUY, TimeInForce::GTC, 101, 1, 1).value()};
    {
        Result<std::unique_ptr<Outbox>> opened{Outbox::Open(dir, profile)};
        RequireOk(opened, "Open new");
        Outbox& outbox{**opened};
        BOOST_CHECK_EQUAL(outbox.Recovery().records, 0U);
        RequireRefusal(outbox.Add("", bid), ErrorCode::INVALID_INPUT, OUTBOX_LABEL_EMPTY, "empty label");
        RequireError(outbox.Add("junk", std::vector<unsigned char>{1, 2, 3}), ErrorCode::DECODE, "bytes that are no action");
        RequireOk(outbox.Add("bid", bid), "Add bid");
        RequireRefusal(outbox.Add("bid", ask), ErrorCode::INVALID_INPUT, OUTBOX_LABEL_TAKEN, "a label twice");
        RequireOk(outbox.Add("ask", ask), "Add ask");
        RequireOk(outbox.Add("bid2", bid2), "Add bid2");
        BOOST_CHECK_EQUAL(outbox.Entries().size(), 3U);
        BOOST_CHECK(outbox.Find("bid").value().bytes == bid);
        BOOST_CHECK(outbox.HighestNonce(rig.alice->Account()) == std::optional<uint64_t>{7});
        BOOST_CHECK(outbox.HighestNonce(rig.bob->Account()) == std::optional<uint64_t>{1});
        BOOST_CHECK(!outbox.HighestNonce(AccountId{}).has_value());
        RequireRefusal(outbox.Record("nope", OutboxResult{OutboxOutcome::INCLUDED, 2, 0, {}}), ErrorCode::INVALID_INPUT, OUTBOX_LABEL_UNKNOWN,
                       "a result for no entry");
        RequireOk(outbox.Record("bid", OutboxResult{OutboxOutcome::INCLUDED, 2, 0, {}}), "Record included");
        RequireRefusal(outbox.Record("bid", OutboxResult{OutboxOutcome::EXPIRED, 3, 0, {}}), ErrorCode::ORDER_VIOLATION, OUTBOX_RESULT_TWICE,
                       "a second result");
        RequireOk(outbox.Record("ask", OutboxResult{OutboxOutcome::FAILED, 4, 0, "NONCE_USED"}), "Record failed");
    }
    // Reopened: the same entries, bytes, order and results; the nonce floor survives.
    Result<std::unique_ptr<Outbox>> reopened{Outbox::Open(dir, profile)};
    RequireOk(reopened, "Open existing");
    const Outbox& outbox{**reopened};
    BOOST_CHECK_EQUAL(outbox.Recovery().records, 5U);
    BOOST_REQUIRE_EQUAL(outbox.Entries().size(), 3U);
    BOOST_CHECK_EQUAL(outbox.Entries()[0].label, "bid");
    BOOST_CHECK(outbox.Entries()[0].bytes == bid);
    const OutboxResult included{OutboxOutcome::INCLUDED, 2, 0, {}};
    const OutboxResult failed{OutboxOutcome::FAILED, 4, 0, "NONCE_USED"};
    BOOST_CHECK(outbox.Entries()[0].result.has_value() && *outbox.Entries()[0].result == included);
    BOOST_CHECK(outbox.Entries()[1].result.has_value() && *outbox.Entries()[1].result == failed);
    BOOST_CHECK(!outbox.Entries()[2].result.has_value());
    BOOST_CHECK(outbox.Entries()[2].action.nonce == 7);
    BOOST_CHECK(outbox.HighestNonce(rig.alice->Account()) == std::optional<uint64_t>{7});
    // The record payloads round-trip through the registry's new types, which only the OUTBOX role accepts.
    BOOST_CHECK(storage::RoleAccepts(storage::JournalRole::OUTBOX, storage::RecordType::OUTBOX_ACTION));
    BOOST_CHECK(storage::RoleAccepts(storage::JournalRole::OUTBOX, storage::RecordType::OUTBOX_RESULT));
    BOOST_CHECK(!storage::RoleAccepts(storage::JournalRole::OUTBOX, storage::RecordType::DECISION));
    BOOST_CHECK(!storage::RoleAccepts(storage::JournalRole::COMMIT, storage::RecordType::OUTBOX_ACTION));
    // A torn tail (a crash while appending) is zeroed by the recovery and the entries before it survive.
    {
        const fs::path segment{dir / fs::u8path(std::string{storage::FIRST_SEGMENT_NAME})};
        std::fstream torn{fs::PathToString(segment), std::ios::in | std::ios::out | std::ios::binary};
        BOOST_REQUIRE(torn.good());
        // The segment is pre-zeroed: find the end of the written records (the first all-zero header) and write a partial header there.
        std::vector<char> image(static_cast<size_t>(OUTBOX_SEGMENT_BYTES));
        torn.read(image.data(), static_cast<std::streamsize>(image.size()));
        size_t end{0};
        while (end + 9 <= image.size()) {
            uint32_t len{0};
            std::memcpy(&len, image.data() + end, 4);
            if (len == 0) break;
            end += 9 + len + 32;
        }
        BOOST_REQUIRE_LT(end + 9, image.size());
        const char partial[9]{0x40, 0x00, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44, 0x0b}; // a 64-byte record announced, never written
        torn.seekp(static_cast<std::streamoff>(end));
        torn.write(partial, sizeof(partial));
        torn.close();
    }
    reopened->reset();
    Result<std::unique_ptr<Outbox>> recovered{Outbox::Open(dir, profile)};
    RequireOk(recovered, "Open after a torn tail");
    BOOST_CHECK_GT((*recovered)->Recovery().truncated_bytes, 0U);
    BOOST_CHECK_EQUAL((*recovered)->Recovery().records, 5U);
    BOOST_REQUIRE_EQUAL((*recovered)->Entries().size(), 3U);
    BOOST_CHECK((*recovered)->Entries()[2].bytes == bid2);
    // The recovery zeroed the tail on disk: a second reopen finds a clean image.
    recovered->reset();
    recovered = Outbox::Open(dir, profile);
    RequireOk(recovered, "Open after the recovery");
    BOOST_CHECK_EQUAL((*recovered)->Recovery().truncated_bytes, 0U);
    BOOST_CHECK_EQUAL((*recovered)->Recovery().records, 5U);
    RequireOk((*recovered)->Add("after", rig.bob->Place("ask2", 2, market, Side::SELL, TimeInForce::GTC, 101, 1, 1).value()), "Add after recovery");
    recovered->reset();
    // A result that names no action is corruption the outbox refuses to open over.
    const fs::path orphan_dir{CliDir("outbox_orphan")};
    {
        Result<std::unique_ptr<storage::FileJournal>> raw{
            storage::FileJournal::CreateNew(orphan_dir, storage::JournalRole::OUTBOX, OUTBOX_INSTANCE, OUTBOX_SEGMENT_BYTES)};
        RequireOk(raw, "CreateNew raw");
        RequireOk((*raw)->Append(storage::RecordType::OUTBOX_RESULT, EncodeOutboxResult("ghost", OutboxResult{OutboxOutcome::INCLUDED, 1, 0, {}})), "Append");
        RequireOk((*raw)->Barrier(), "Barrier");
    }
    RequireRefusal(Outbox::Open(orphan_dir, profile), ErrorCode::ORDER_VIOLATION, OUTBOX_RESULT_ORPHAN, "a result without its action");
    fs::remove_all(orphan_dir);
    fs::remove_all(dir);
}

BOOST_AUTO_TEST_CASE(a_failed_result_in_the_outbox_gives_way_to_a_certified_one)
{
    // Review workflow of 2026-11-18 (08a): 4da8c4d2 stopped fm2-cli recording an uncertified FAILED, but an outbox an
    // earlier build wrote may hold one. `resend --wait` then printed the stale FAILED without polling, and had it polled,
    // recording the certified result would have been OUTBOX_RESULT_TWICE. A FAILED is never final: a certified result
    // (INCLUDED or EXPIRED) may follow it, and survives a reopen; after a certified result nothing more is taken.
    Rig rig;
    const fs::path dir{CliDir("outbox_failed")};
    const ProfileParams& profile{rig.node->Profile()};
    const MarketDescriptor market{rig.node->Markets().front()};
    const OutboxResult failed{OutboxOutcome::FAILED, 4, 0, "NONCE_USED"};
    const OutboxResult included{OutboxOutcome::INCLUDED, 6, 1, {}};
    {
        Result<std::unique_ptr<Outbox>> opened{Outbox::Open(dir, profile)};
        RequireOk(opened, "Open new");
        RequireOk((*opened)->Add("bid", rig.alice->Place("bid", 1, market, Side::BUY, TimeInForce::GTC, 100, 5, 1).value()), "Add bid");
        RequireOk((*opened)->Record("bid", failed), "Record failed (as an earlier build did)");
    }
    {
        Result<std::unique_ptr<Outbox>> opened{Outbox::Open(dir, profile)};
        RequireOk(opened, "Reopen");
        BOOST_REQUIRE((*opened)->Find("bid").value().result == std::optional<OutboxResult>{failed});
        RequireOk((*opened)->Record("bid", included), "a certified result after a FAILED");
        RequireRefusal((*opened)->Record("bid", OutboxResult{OutboxOutcome::EXPIRED, 7, 0, {}}), ErrorCode::ORDER_VIOLATION, OUTBOX_RESULT_TWICE,
                       "a result after a certified one");
    }
    Result<std::unique_ptr<Outbox>> reopened{Outbox::Open(dir, profile)};
    RequireOk(reopened, "Reopen after the certified result");
    BOOST_CHECK((*reopened)->Find("bid").value().result == std::optional<OutboxResult>{included});
    reopened->reset();
    fs::remove_all(dir);
}

BOOST_AUTO_TEST_CASE(the_chunk_decoder_is_incremental_and_refuses_malformed_sizes)
{
    // Chunks split anywhere across reads decode to the same body; the terminal chunk ends it.
    const std::string body{"5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n"};
    for (size_t cut{1}; cut < body.size(); ++cut) {
        ChunkDecoder decoder;
        decoder.Feed(std::string_view{body}.substr(0, cut));
        decoder.Feed(std::string_view{body}.substr(cut));
        BOOST_CHECK_MESSAGE(decoder.Decoded() == "hello world" && decoder.Done() && !decoder.Malformed(), "cut at " << cut);
    }
    // Take drops consumed bytes; a chunk extension is ignored.
    ChunkDecoder taking;
    taking.Feed("3;ext=1\r\nabc\r\n");
    BOOST_CHECK_EQUAL(taking.Decoded(), "abc");
    taking.Take(2);
    BOOST_CHECK_EQUAL(taking.Decoded(), "c");
    // A negative, non-hex or oversize size, a missing CRLF after the data, or an endless size line are malformed, not trusted.
    for (const char* bad : {"-1\r\n", "zz\r\n", "fffffffffffffff\r\n", "100001\r\n", "3\r\nabcXY", "\r\n"}) {
        ChunkDecoder decoder;
        decoder.Feed(bad);
        BOOST_CHECK_MESSAGE(decoder.Malformed(), "accepted: " << bad);
    }
    ChunkDecoder endless;
    endless.Feed(std::string(40, 'a'));
    BOOST_CHECK(endless.Malformed());
    // The head parser.
    size_t body_start{0};
    const std::optional<std::pair<int, std::string>> head{HttpsClient::ParseHead("HTTP/1.1 429 Too Many\r\nX: y\r\n\r\nbody", body_start)};
    BOOST_REQUIRE(head.has_value());
    BOOST_CHECK_EQUAL(head->first, 429);
    BOOST_CHECK_EQUAL(body_start, 31U);
    BOOST_CHECK(!HttpsClient::ParseHead("HTTP/1.1 200 OK\r\nX: y", body_start).has_value());
    for (const char* odd : {"HTTP/1.1 -12 x\r\n\r\n", "HTTP/1.1 20\r\nX: y\r\n\r\n", "HTTP/1.1 2000 OK\r\n\r\n", "HTTP/1.1 099 x\r\n\r\n", "HTTP/1.1 600 x\r\n\r\n"}) {
        const std::optional<std::pair<int, std::string>> bad{HttpsClient::ParseHead(odd, body_start)};
        BOOST_CHECK_MESSAGE(bad.has_value() && bad->first == 0, "accepted status line: " << odd);
    }
    BOOST_CHECK(HttpsClient::ContentLength("HTTP/1.1 200 OK\r\nContent-Length: 42\r\nX: y") == std::optional<size_t>{42});
    BOOST_CHECK(HttpsClient::ContentLength("HTTP/1.1 200 OK\r\ncontent-length:7") == std::optional<size_t>{7});
    BOOST_CHECK(!HttpsClient::ContentLength("HTTP/1.1 200 OK\r\nX: y").has_value());
}

BOOST_AUTO_TEST_CASE(the_key_signs_once_and_the_account_follows_it)
{
    const fs::path dir{CliDir("key")};
    const fs::path file{dir / fs::u8path("client.key")};
    const Result<ClientKey> made{ClientKey::Generate(file)};
    RequireOk(made, "Generate");
    RequireRefusal(ClientKey::Generate(file), ErrorCode::INVALID_INPUT, KEY_EXISTS, "a second Generate");
    const Result<ClientKey> loaded{ClientKey::Load(file)};
    RequireOk(loaded, "Load");
    BOOST_CHECK(loaded->Account() == made->Account());
    BOOST_CHECK(loaded->Account() == AccountId::FromKey(loaded->Key()));
    RequireRefusal(ClientKey::Load(dir / fs::u8path("missing.key")), ErrorCode::DECODE, KEY_FILE, "a missing file");
    Rig rig;
    ActionV2 action;
    action.domain = rig.node->Domain();
    action.nonce = 1;
    action.ref_slot = 1;
    action.max_delay = 16;
    action.kind = ActionKind::CANCEL_ALL;
    const Result<std::vector<unsigned char>> bytes{loaded->SignAction(action)};
    RequireOk(bytes, "SignAction");
    const Result<ActionV2> decoded{ActionV2::Decode(*bytes, rig.node->Profile())};
    RequireOk(decoded, "Decode");
    BOOST_CHECK(decoded->signer == loaded->Key());
    BOOST_CHECK(decoded->Account() == loaded->Account());
    BOOST_CHECK(decoded->VerifySignature());
    // The same action signed again is the same action (same id) under a different signature (fresh aux randomness): the
    // outbox keeps the first bytes and refuses the label again, so the client never posts two signatures of one action.
    const Result<std::vector<unsigned char>> again{loaded->SignAction(action)};
    RequireOk(again, "SignAction again");
    BOOST_CHECK(ActionV2::Decode(*again, rig.node->Profile())->Id() == decoded->Id());
    BOOST_CHECK(*again != *bytes);
    {
        Result<std::unique_ptr<Outbox>> outbox{Outbox::Open(dir / fs::u8path("outbox"), rig.node->Profile())};
        RequireOk(outbox, "Open");
        RequireOk((*outbox)->Add("once", *bytes), "Add the first signature");
        RequireRefusal((*outbox)->Add("once", *again), ErrorCode::INVALID_INPUT, OUTBOX_LABEL_TAKEN, "the second signature under the same label");
        BOOST_CHECK((*outbox)->Find("once").value().bytes == *bytes);
    }
    // A key file readable by others is refused.
    const fs::path loose{dir / fs::u8path("loose.key")};
    fs::copy_file(file, loose, fs::copy_options::none);
    fs::permissions(loose, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read);
    RequireRefusal(ClientKey::Load(loose), ErrorCode::INVALID_INPUT, KEY_MODE, "a key file readable by the group");
    fs::remove_all(dir);
}

BOOST_AUTO_TEST_CASE(replies_rebuild_from_json_and_verify_against_the_trust_inputs)
{
    Rig rig;
    const Trust trust{rig.TrustOf()};
    const ProfileParams& profile{rig.node->Profile()};
    // A state reply: the JSON the API renders parses back to the same reply and verifies.
    const Result<read::AccountReply> state{rig.node->Api().GetState(rig.alice->Account())};
    RequireOk(state, "GetState");
    const Result<UniValue> state_json{ParseJson(Text(netio::AccountReplyJson(*state)))};
    RequireOk(state_json, "state JSON");
    const Result<StateReply> parsed{ParseStateReply(*state_json, profile)};
    RequireOk(parsed, "ParseStateReply");
    BOOST_CHECK(parsed->found);
    BOOST_CHECK(parsed->head.envelope == state->head.envelope && parsed->head.commit == state->head.commit && parsed->head.scope == state->head.scope);
    BOOST_REQUIRE(parsed->witness.has_value());
    BOOST_CHECK(*parsed->witness == *state->witness);
    RequireOk(trust.VerifyHead(parsed->head), "VerifyHead");
    const Result<AccountLeafV0> leaf{trust.VerifyAccount(parsed->head, *parsed->witness, rig.alice->Account())};
    RequireOk(leaf, "VerifyAccount");
    BOOST_CHECK_EQUAL(leaf->Available(SimAsset("rUSD")), 1'000'000'000);
    RequireRefusal(trust.VerifyAccount(parsed->head, *parsed->witness, rig.bob->Account()), ErrorCode::VERIFY_FAILED, TRUST_WRONG_ACCOUNT,
                   "another account's leaf");
    // A tampered envelope (one certificate byte) does not verify; a tampered commit identity does not either.
    read::ReplyHead forged{parsed->head};
    forged.envelope.cert.aggregate[3] ^= 0x01;
    BOOST_CHECK(!trust.VerifyHead(forged).has_value());
    read::ReplyHead claimed{parsed->head};
    claimed.commit.sequence += 1;
    RequireRefusal(trust.VerifyHead(claimed), ErrorCode::VERIFY_FAILED, TRUST_HEAD_COMMIT, "a commit identity that is not the header's");
    // A missing field and a wrong hex are DECODE.
    UniValue broken{*state_json};
    broken.pushKV("head", UniValue{UniValue::VOBJ});
    RequireRefusal(ParseStateReply(broken, profile), ErrorCode::DECODE, JSON_SHAPE, "a head without fields");
    UniValue bad_hex{*state_json};
    UniValue head{(*state_json)["head"]};
    head.pushKV("envelope", UniValue{"zz"});
    bad_hex.pushKV("head", head);
    RequireRefusal(ParseStateReply(bad_hex, profile), ErrorCode::DECODE, JSON_HEX, "an envelope that is not hex");
    // An included outcome: bob's ask rests, then alice's bid fills it; the status JSON rebuilds and verifies, with its fills.
    const MarketDescriptor market{rig.node->Markets().front()};
    const std::vector<unsigned char> ask{rig.bob->Place("ask", 1, market, Side::SELL, TimeInForce::GTC, 100, 5, 1).value()};
    RequireOk(rig.node->Api().Submit(ask), "Submit ask");
    (void)rig.node->Pump(16);
    RequireOk(rig.node->SealSlot(), "SealSlot 2");
    const std::vector<unsigned char> bid{rig.alice->Place("bid", 1, market, Side::BUY, TimeInForce::GTC, 100, 5, 2).value()};
    RequireOk(rig.node->Api().Submit(bid), "Submit bid");
    (void)rig.node->Pump(16);
    RequireOk(rig.node->SealSlot(), "SealSlot 3");
    const ActionV2 bid_action{ActionV2::Decode(bid, profile).value()};
    const Result<net::ActionStatusReply> status{rig.node->Api().GetAction(bid_action)};
    RequireOk(status, "GetAction");
    BOOST_REQUIRE(status->status == net::ApiActionStatus::INCLUDED);
    const Result<UniValue> status_json{ParseJson(Text(netio::ActionStatusJson(*status)))};
    RequireOk(status_json, "status JSON");
    const Result<StatusReply> parsed_status{ParseStatusReply(*status_json, profile)};
    RequireOk(parsed_status, "ParseStatusReply");
    BOOST_CHECK_EQUAL(parsed_status->status, "INCLUDED");
    BOOST_REQUIRE(parsed_status->included.has_value());
    BOOST_CHECK(parsed_status->included->ref == status->included->ref && parsed_status->included->envelope == status->included->envelope &&
                parsed_status->included->outcome == status->included->outcome);
    const Result<OutcomeV2> outcome{trust.VerifyIncluded(*parsed_status->included, bid_action.Id())};
    RequireOk(outcome, "VerifyIncluded");
    BOOST_CHECK_EQUAL(outcome->fills.size(), 1U);
    BOOST_CHECK_EQUAL(outcome->fills.front().lots, 5U);
    RequireRefusal(trust.VerifyIncluded(*parsed_status->included, ActionV2::Decode(ask, profile)->Id()), ErrorCode::VERIFY_FAILED,
                   TRUST_OUTCOME_SUBJECT, "another action's outcome");
    // A health reply.
    const Result<UniValue> health_json{ParseJson("{\"fenced\": false, \"published_height\": 3, \"pool_live\": 0}")};
    const Result<HealthReplyJson> health{ParseHealthReply(*health_json)};
    RequireOk(health, "ParseHealthReply");
    BOOST_CHECK(health->published_height == std::optional<uint64_t>{3});
}

BOOST_AUTO_TEST_CASE(the_pinned_client_refuses_another_certificate_and_talks_to_the_right_one)
{
    Rig rig;
    rig.Serve();
    const uint16_t port{rig.api->Port()};
    const std::string pin{rig.api->CertificateFingerprint()};
    BOOST_REQUIRE(HttpsClient::PinWellFormed(pin));
    BOOST_CHECK(!HttpsClient::PinWellFormed("abc"));
    // The wrong pin: the handshake completes, the key is not the pinned one, nothing is sent.
    std::string other{pin};
    other[0] = other[0] == 'a' ? 'b' : 'a';
    HttpsClient wrong{"127.0.0.1", port, other, 2'000};
    RequireRefusal(wrong.Request("GET", "/v2/health"), ErrorCode::VERIFY_FAILED, HTTPS_PIN, "another certificate");
    // A malformed pin never connects; a closed port is BUSY.
    HttpsClient malformed{"127.0.0.1", port, "zz", 2'000};
    RequireRefusal(malformed.Request("GET", "/v2/health"), ErrorCode::INVALID_INPUT, HTTPS_PIN_SHAPE, "a malformed pin");
    HttpsClient nobody{"127.0.0.1", 1, pin, 500};
    RequireRefusal(nobody.Request("GET", "/v2/health"), ErrorCode::BUSY, HTTPS_CONNECT, "a closed port");
    // The right pin: health, a state read that verifies, an action posted and found, and a stream.
    const Trust trust{rig.TrustOf()};
    HttpsClient client{"127.0.0.1", port, pin, 5'000};
    const Result<HttpResponse> health{client.Request("GET", "/v2/health")};
    RequireOk(health, "GET /v2/health");
    BOOST_CHECK_EQUAL(health->status, 200);
    RequireOk(ParseHealthReply(ParseJson(health->body).value()), "health JSON");
    const Result<HttpResponse> state{client.Request("GET", "/v2/state/" + rig.alice->Account().id.GetHex())};
    RequireOk(state, "GET /v2/state");
    BOOST_REQUIRE_EQUAL(state->status, 200);
    const Result<StateReply> parsed{ParseStateReply(ParseJson(state->body).value(), rig.node->Profile())};
    RequireOk(parsed, "ParseStateReply over TLS");
    RequireOk(trust.VerifyAccount(parsed->head, *parsed->witness, rig.alice->Account()), "VerifyAccount over TLS");
    const MarketDescriptor market{rig.node->Markets().front()};
    const std::vector<unsigned char> bid{rig.alice->Place("bid", 1, market, Side::BUY, TimeInForce::GTC, 100, 5, 1).value()};
    const Result<HttpResponse> posted{client.Request("POST", "/v2/actions", bid)};
    RequireOk(posted, "POST /v2/actions");
    BOOST_CHECK_EQUAL(posted->status, 200);
    const Result<AdmissionReplyJson> admitted{ParseAdmissionReply(ParseJson(posted->body).value(), rig.node->Profile())};
    RequireOk(admitted, "admission JSON");
    BOOST_CHECK_EQUAL(admitted->code, "ADMITTED");
    const Result<HttpResponse> again{client.Request("POST", "/v2/actions", bid)};
    RequireOk(again, "POST again");
    BOOST_CHECK_EQUAL(ParseAdmissionReply(ParseJson(again->body).value(), rig.node->Profile())->code, "DUPLICATE");
    const Result<HttpResponse> status{client.Request("POST", "/v2/actions/status", bid)};
    RequireOk(status, "POST /v2/actions/status");
    BOOST_CHECK_EQUAL(ParseStatusReply(ParseJson(status->body).value(), rig.node->Profile())->status, "PENDING");
    std::vector<SseEvent> events;
    const Result<HttpResponse> streamed{client.Stream("/v2/stream?channel=slots", [&](const SseEvent& event) {
        events.push_back(event);
        return events.size() < 1;
    }, 3'000)};
    RequireOk(streamed, "Stream");
    BOOST_CHECK_EQUAL(streamed->status, 200);
    // A refused stream hands the server's error back instead of pretending.
    const Result<HttpResponse> refused{client.Stream("/v2/stream?channel=account", [&](const SseEvent&) { return true; }, 3'000)};
    RequireOk(refused, "Stream refused");
    BOOST_CHECK_EQUAL(refused->status, 400);
    BOOST_CHECK(refused->body.find("error") != std::string::npos);
    BOOST_REQUIRE_EQUAL(events.size(), 1U);
    const Result<UniValue> data{ParseJson(events.front().data)};
    RequireOk(data, "event data");
    BOOST_CHECK_EQUAL((*data)["channel"].get_str(), "slots");
    const Result<read::CertifiedEnvelope> envelope{read::CertifiedEnvelope::Decode(ParseBytes((*data)["envelope"]).value())};
    RequireOk(envelope, "event envelope");
    RequireOk(trust.VerifyEnvelope(*envelope), "VerifyEnvelope of the streamed slot");
}

BOOST_AUTO_TEST_SUITE_END()
