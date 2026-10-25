// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// TODO(rebirth:M4-DEV-01) regtest operator worker; chain facts arrive only through
// the wrapper-fed observe command until the b3d adapter (MB1-ADP-06) delivers them.
#include <flowmesh2/dev/regtest_operator.h>

#include <crypto/bls.h>
#include <crypto/keccak256.h>
#include <crypto/sha256.h>
#include <flowmesh2/admission/admission.h>
#include <flowmesh2/admission/pool.h>
#include <flowmesh2/agreement/pbft.h>
#include <flowmesh2/agreement/control_messages.h>
#include <flowmesh2/chain/chain_adapter.h>
#include <flowmesh2/core/action.h>
#include <flowmesh2/core/body.h>
#include <flowmesh2/core/certificate.h>
#include <flowmesh2/core/facts.h>
#include <flowmesh2/core/hashes.h>
#include <flowmesh2/core/merkle.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/seat_set.h>
#include <flowmesh2/dev/dev_budget.h>
#include <flowmesh2/ledger/state_commitment.h>
#include <flowmesh2/net/tcp_mesh.h>
#include <flowmesh2/read/read_service.h>
#include <flowmesh2/read/views.h>
#include <flowmesh2/state/state_owner.h>
#include <flowmesh2/storage/commit_log.h>
#include <flowmesh2/storage/control_log.h>
#include <flowmesh2/storage/file_journal.h>
#include <flowmesh2/storage/journal_commit.h>
#include <flowmesh2/storage/seat_signer.h>
#include <flowmesh2/storage/signer_log.h>
#include <flowmesh2/verify/b3verify.h>
#include <key.h>
#include <streams.h>
#include <util/fs.h>
#include <util/strencodings.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace flowmesh2::dev {
namespace {
using Bytes = std::vector<unsigned char>;

[[noreturn]] void Fail(ErrorCode code, std::string_view detail, bool fatal = false)
{
    throw OperatorFailure{code, std::string{detail}, fatal};
}
void Need(bool condition, std::string_view detail)
{
    if (!condition) Fail(ErrorCode::INVALID_INPUT, detail);
}
template <class T> T Must(Result<T> value)
{
    if (!value.has_value()) Fail(value.error().code, value.error().detail,
                                value.error().code == ErrorCode::STORAGE_FATAL || value.error().code == ErrorCode::FENCED);
    return std::move(value).value();
}
void Must(Result<void> value)
{
    if (!value.has_value()) Fail(value.error().code, value.error().detail,
                                value.error().code == ErrorCode::STORAGE_FATAL || value.error().code == ErrorCode::FENCED);
}
std::string Text(const UniValue& object, const std::string& key)
{
    Need(object.isObject() && object[key].isStr(), "missing/string JSON field");
    return object[key].get_str();
}
template <class T> T Number(const UniValue& object, const std::string& key)
{
    Need(object.isObject() && object[key].isNum(), "missing/integer JSON field");
    try { return object[key].getInt<T>(); }
    catch (const std::exception&) { Fail(ErrorCode::INVALID_INPUT, "integer field out of range"); }
}
Bytes Hex(const UniValue& object, const std::string& key, size_t maximum, std::optional<size_t> exact = {})
{
    std::string text{Text(object, key)};
    if (text.starts_with("0x")) text.erase(0, 2);
    Need(text.size() % 2 == 0 && text.size() / 2 <= maximum && (text.empty() || IsHex(text)), "invalid/bounded hex field");
    Need(!exact || text.size() / 2 == *exact, "hex field has wrong fixed width");
    return ParseHex(text);
}
uint256 JsonHash(const UniValue& object, const std::string& key)
{
    const Bytes bytes{Hex(object, key, 32, 32)};
    uint256 value;
    std::copy(bytes.begin(), bytes.end(), value.begin());
    return value;
}
NitroLabBinding ReadNitroLab(const UniValue& value)
{
    const std::set<std::string> expected{"source_chain_id", "source_genesis_hash", "quote_token", "quote_token_code_hash",
                                         "quote_implementation", "quote_implementation_code_hash"};
    Need(value.isObject() && value.size() == expected.size(), "exact owned Nitro lab binding required");
    const auto& keys{value.getKeys()};
    Need(std::set<std::string>{keys.begin(), keys.end()} == expected, "unknown/duplicate owned Nitro lab binding field");
    Need(Number<uint64_t>(value, "source_chain_id") == NITRO_LAB_CHAIN_ID, "owned Nitro lab source must be chain412346");
    NitroLabBinding out;
    out.source_genesis_hash = JsonHash(value, "source_genesis_hash");
    const auto token{Hex(value, "quote_token", 20, 20)}, implementation{Hex(value, "quote_implementation", 20, 20)};
    std::copy(token.begin(), token.end(), out.quote_token.begin());
    std::copy(implementation.begin(), implementation.end(), out.quote_implementation.begin());
    out.quote_token_code_hash = JsonHash(value, "quote_token_code_hash");
    out.quote_implementation_code_hash = JsonHash(value, "quote_implementation_code_hash");
    return out;
}
UniValue NitroLabJson(const NitroLabBinding& value)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("source_chain_id", NITRO_LAB_CHAIN_ID); out.pushKV("source_genesis_hash", HexStr(value.source_genesis_hash));
    out.pushKV("quote_token", HexStr(value.quote_token)); out.pushKV("quote_token_code_hash", HexStr(value.quote_token_code_hash));
    out.pushKV("quote_implementation", HexStr(value.quote_implementation));
    out.pushKV("quote_implementation_code_hash", HexStr(value.quote_implementation_code_hash));
    return out;
}
FactCursor Cursor(const UniValue& value)
{
    return FactCursor{Number<uint32_t>(value, "height"), Number<uint32_t>(value, "index")};
}
Anchor ReadAnchor(const UniValue& value)
{
    return Anchor{Number<uint32_t>(value, "height"), JsonHash(value, "hash")};
}
UniValue CursorJson(const FactCursor& cursor)
{
    UniValue out{UniValue::VOBJ}; out.pushKV("height", cursor.height); out.pushKV("index", cursor.index); return out;
}
UniValue ProofJson(const MerkleProof& proof)
{
    UniValue out{UniValue::VOBJ}, path{UniValue::VARR};
    out.pushKV("index", proof.index); out.pushKV("size", proof.size);
    for (const auto& hash : proof.path) path.push_back(HexStr(hash));
    out.pushKV("path", std::move(path)); return out;
}
UniValue RootsJson(const StateRootsV0& roots)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("version", roots.version); out.pushKV("accounts", HexStr(roots.accounts));
    out.pushKV("orders", HexStr(roots.orders)); out.pushKV("markets", HexStr(roots.markets));
    out.pushKV("nullifiers", HexStr(roots.nullifiers)); out.pushKV("totals", HexStr(roots.totals));
    out.pushKV("withdrawals", HexStr(roots.withdrawals)); return out;
}
AssetId NamedAsset(std::string_view name)
{
    return AssetId{Tagged(HashTag::TEST_ASSET, std::span<const unsigned char>{
        reinterpret_cast<const unsigned char*>(name.data()), name.size()})};
}
CKey ClientKey(const UniValue& request)
{
    const Bytes secret{Hex(request, "seed", 32, 32)};
    CKey key; key.Set(secret.begin(), secret.end(), true);
    Need(key.IsValid(), "invalid local test secret"); return key;
}
std::string EvmAddress(const CKey& key)
{
    CPubKey pub{key.GetPubKey()}; Need(pub.Decompress() && pub.size() == 65, "uncompressed public key failed");
    std::array<unsigned char, 32> digest{};
    Keccak256{}.Write(std::span<const unsigned char>{pub.data() + 1, 64}).Finalize(digest);
    return "0x" + HexStr(std::span<const unsigned char>{digest}.subspan(12));
}
Bytes ReadIdentity(const fs::path& file)
{
    std::error_code ec;
    Need(fs::is_regular_file(fs::symlink_status(file, ec)) && !ec, "existing store identity missing or not regular");
    Need(fs::file_size(file, ec) <= 16384 && !ec, "store identity exceeds bound");
    std::ifstream stream{file.std_path(), std::ios::binary};
    if (!stream.good()) Fail(ErrorCode::STORAGE_FATAL, "cannot read original store identity", true);
    return Bytes{std::istreambuf_iterator<char>{stream}, {}};
}
void CreateIdentity(const fs::path& file, std::string_view text)
{
    const int fd{open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
    if (fd < 0) Fail(ErrorCode::STORAGE_FATAL, "cannot exclusively create store identity", true);
    size_t offset{0};
    while (offset < text.size()) {
        const ssize_t count{write(fd, text.data() + offset, text.size() - offset)};
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { close(fd); Fail(ErrorCode::STORAGE_FATAL, "store identity write failed", true); }
        offset += count;
    }
    const auto committed{storage::JournalCommit(fd)};
    const int closed{close(fd)};
    Must(committed);
    if (closed != 0) Fail(ErrorCode::STORAGE_FATAL, "store identity close failed", true);
    Must(storage::CommitDirectory(file.parent_path()));
}
std::vector<Bytes> Frames(const UniValue& array, size_t count_cap, size_t byte_cap)
{
    Need(array.isArray() && array.size() <= count_cap, "frame array exceeds cap");
    std::vector<Bytes> out;
    out.reserve(array.size());
    for (const auto& item : array.getValues()) {
        UniValue holder{UniValue::VOBJ}; holder.pushKV("hex", item);
        out.push_back(Hex(holder, "hex", byte_cap));
    }
    return out;
}
} // namespace

UniValue RegtestNitroLabProfile(const UniValue& request)
{
    const auto profile{Must(NitroLabRegtestProfile(ChainType::REGTEST, ReadNitroLab(request["nitro_lab"])))};
    UniValue out{UniValue::VOBJ};
    out.pushKV("profile_name", profile.profile_id); out.pushKV("profile_hash", HexStr(ProfileHash(profile)));
    out.pushKV("encoding", NITRO_LAB_PROFILE_ENCODING_VERSION); out.pushKV("quote_asset", HexStr(profile.stable_fee_asset->id));
    out.pushKV("encoded_profile", HexStr(EncodeProfile(profile))); out.pushKV("nitro_lab", NitroLabJson(*profile.nitro_lab));
    out.pushKV("lab_only", true); out.pushKV("mainnet_enabled", false); out.pushKV("rollup_settlement_verified", false);
    return out;
}

UniValue RegtestKeyInfo(const UniValue& request)
{
    const auto key{bls::SecretKey::FromIKM(Hex(request, "ikm", 64, 32))};
    Need(key.has_value(), "BLS test key generation failed");
    UniValue out{UniValue::VOBJ};
    out.pushKV("public_key", HexStr(key->GetPublicKey().Compressed()));
    out.pushKV("pop", HexStr(key->SignPoP().Compressed())); return out;
}

UniValue RegtestClientAction(const UniValue& request)
{
    CKey key{ClientKey(request)};
    ActionV2 action; action.domain = JsonHash(request, "domain");
    action.signer = XOnlyPubKey{key.GetPubKey()};
    action.nonce = Number<uint64_t>(request, "nonce"); action.ref_slot = Number<uint64_t>(request, "ref_slot");
    action.max_delay = request.exists("max_delay") ? Number<uint16_t>(request, "max_delay") : 32;
    const auto profile_name{request.exists("profile_name") ? Text(request, "profile_name") : std::string{STABLE_Q_PROFILE_ID}};
    Need(profile_name == STABLE_Q_PROFILE_ID || profile_name == STABLE_Q_VIEW_PROFILE_ID || profile_name == STABLE_Q_NETWORK_PROFILE_ID ||
         profile_name == NITRO_LAB_PROFILE_ID, "unknown client helper profile");
    Need(profile_name == NITRO_LAB_PROFILE_ID || !request.exists("nitro_lab"), "lab binding cannot reinterpret an old client profile");
    const auto profile{Must(profile_name == NITRO_LAB_PROFILE_ID ? NitroLabRegtestProfile(ChainType::REGTEST, ReadNitroLab(request["nitro_lab"])) :
        profile_name == STABLE_Q_NETWORK_PROFILE_ID ? StableQNetworkRegtestProfile(ChainType::REGTEST) :
        profile_name == STABLE_Q_VIEW_PROFILE_ID ? StableQViewRegtestProfile(ChainType::REGTEST) : StableQRegtestProfile(ChainType::REGTEST))};
    Need(!action.domain.IsNull() && action.MaxDelayValid(profile), "invalid action domain/window");
    const UniValue& p{request["payload"]};
    const std::string kind{Text(request, "kind")};
    if (kind == "PLACE") {
        const std::string side{Text(p, "side")}, tif{Text(p, "tif")};
        Need(side == "BUY" || side == "SELL", "invalid side");
        Need(tif == "GTC" || tif == "IOC" || tif == "POST_ONLY", "invalid time in force");
        action.kind = ActionKind::PLACE;
        action.payload = Must(EncodePayload(PlacePayload{MarketId{JsonHash(p, "market")}, side == "BUY" ? Side::BUY : Side::SELL,
            tif == "GTC" ? TimeInForce::GTC : tif == "IOC" ? TimeInForce::IOC : TimeInForce::POST_ONLY,
            Number<uint64_t>(p, "price_ticks"), Number<uint64_t>(p, "lots")}));
    } else if (kind == "WITHDRAW") {
        action.kind = ActionKind::WITHDRAW;
        action.payload = Must(EncodePayload(WithdrawPayload{AssetId{JsonHash(p, "asset")}, Number<Atoms>(p, "amount"), Hex(p, "recipient", 20, 20)}));
    } else if (kind == "TRANSFER") {
        action.kind = ActionKind::TRANSFER;
        action.payload = Must(EncodePayload(TransferPayload{AccountId{JsonHash(p, "to")}, AssetId{JsonHash(p, "asset")}, Number<Atoms>(p, "amount")}));
    } else if (kind == "CANCEL") {
        action.kind = ActionKind::CANCEL; action.payload = Must(EncodePayload(CancelPayload{OrderId{JsonHash(p, "order")}}));
    } else Fail(ErrorCode::INVALID_INPUT, "unsupported client helper kind");
    Need(key.SignSchnorr(action.Id().id, action.sig, nullptr, uint256{}), "test BIP340 signing failed");
    Need(action.VerifySignature(), "test BIP340 signature self-check failed");
    UniValue out{UniValue::VOBJ}; out.pushKV("frame", HexStr(action.Encode())); out.pushKV("id", HexStr(action.Id().id));
    out.pushKV("account", HexStr(action.Account().id)); out.pushKV("signer", HexStr(action.signer)); return out;
}

UniValue RegtestNetworkKeyInfo(const UniValue& request)
{
    const CKey key{ClientKey(request)};
    UniValue out{UniValue::VOBJ}; out.pushKV("public_key", HexStr(XOnlyPubKey{key.GetPubKey()})); return out;
}

UniValue RegtestEvmSign(const UniValue& request)
{
    CKey key{ClientKey(request)};
    std::vector<unsigned char> compact;
    Need(key.SignCompact(JsonHash(request, "digest"), compact) && compact.size() == 65, "test ECDSA signing failed");
    const unsigned int recovery{static_cast<unsigned int>(compact[0] - 27) & 3U};
    Need(recovery <= 1, "Ethereum cannot encode this recovery id");
    Bytes signature(compact.begin() + 1, compact.end()); signature.push_back(27 + recovery);
    UniValue out{UniValue::VOBJ}; out.pushKV("signature", HexStr(signature));
    out.pushKV("address", EvmAddress(key)); return out;
}

UniValue RegtestEvmKeygen()
{
    CKey key; key.MakeNewKey(true);
    Need(key.IsValid(), "random ECDSA key generation failed");
    UniValue out{UniValue::VOBJ}; out.pushKV("seed", HexStr(std::span{key.data(), key.size()}));
    out.pushKV("address", EvmAddress(key)); return out;
}

UniValue RegtestEvmKeyInfo(const UniValue& request)
{
    const CKey key{ClientKey(request)};
    UniValue out{UniValue::VOBJ}; out.pushKV("address", EvmAddress(key)); return out;
}

struct RegtestOperator::Impl {
    ProfileParams profile{Must(StableQRegtestProfile(ChainType::REGTEST))};
    AssetId a{NamedAsset("TOKENA")}, b{NamedAsset("TOKENB")}, q{RegtestQuoteAsset()};
    std::vector<MarketDescriptor> markets{{a, q, 1'000'000, 10'000}, {b, q, 1'000'000, 10'000}};
    uint32_t seat{0};
    uint64_t signer_instance{0}, commit_instance{0}, control_instance{0};
    std::optional<SeatSet> seats;
    AuthorityVersion authority;
    uint256 domain;
    fs::path directory;
    std::unique_ptr<storage::FileJournal> signer_journal, commit_journal, control_journal;
    std::unique_ptr<storage::SignerLog> signer_log;
    std::unique_ptr<storage::CommitLog> commit_log;
    std::unique_ptr<storage::ControlLog> control_log;
    std::unique_ptr<storage::SeatSigner> signer;
    std::optional<state::HeadView> genesis;
    std::unique_ptr<state::StateOwner> owner;
    std::unique_ptr<read::ReadService> read;
    std::unique_ptr<admission::ActionPool> pool;
    std::unique_ptr<UnlimitedDevBudget> budget;
    std::unique_ptr<admission::Admission> admission;
    std::map<uint64_t, SlotCertificate> certificates;
    std::map<uint64_t, storage::DecisionRecord> decisions;
    std::optional<state::SlotInputs> observed;
    std::optional<state::SlotInputs> recovered_observation;
    bool batch_locked{false};
    bool critical{false};
    bool pbft_mode{false};
    bool pbft_view_change{false};
    bool pbft_timed_out{false};
    std::optional<SlotHeaderV2> pbft_header;
    std::map<storage::ProposalKey, uint256> accepted_headers;
    bool network_mode{false};
    bool nitro_lab_mode{false};
    std::optional<CKey> network_key;
    UniValue network_manifest;
    std::unique_ptr<net::SeatMesh> mesh;
    uint64_t next_network_token{0};
    std::map<uint64_t, net::InboundFrame> network_pending;
    static constexpr size_t NETWORK_PENDING_LIMIT{64};
    uint64_t network_controller_generation{0};
    struct ObservationAuthority {
        uint64_t token{0};
        agreement::ControlNewView proof;
        Bytes message;
        bool restored{false};
        bool fenced{false};
        bool own_quorum{false};
        std::optional<agreement::CertifiedProposal> history;
        const agreement::Proposal& Selected() const { return history ? history->proposal : *proof.selected_proposal; }
        uint64_t View() const { return history ? history->certificate.view.value_or(0) : proof.key.view; }
    };
    std::optional<ObservationAuthority> observation_authority;
    std::map<ActionId, UniValue> withdrawal_authorities;

    // The wrapper is the observer/chain boundary for this local fixture. It
    // checks actual RPC ancestry, finality, receipts and complete ranges before
    // this private adapter is populated. It cannot synthesize deposit credit.
    struct ObservedAdapter final : chain::ChainAdapter {
        Impl& source;
        explicit ObservedAdapter(Impl& source_in) : source{source_in} {}
        NetworkId Network() const override { return NetworkId{ChainType::REGTEST, {}, source.domain}; }
        AuthorityVersion CurrentAuthority() const override { return source.authority; }
        Result<SeatSet> SeatSetAt(uint64_t epoch) const override
        {
            if (epoch != 0) return Error{ErrorCode::NEED_DATA, "operator has only pinned epoch zero"};
            return Ok(*source.seats);
        }
        Result<chain::PollReply> Poll(const FactCursor& after, size_t maximum) override
        {
            if (maximum == 0 || maximum > MAX_SLOT_FACTS) return Error{ErrorCode::INVALID_INPUT, chain::POLL_MAX_FACTS};
            if (!source.observed) return Ok(chain::PollReply::NeedData(source.authority));
            const auto& inputs{*source.observed};
            chain::FactBatch batch{inputs.facts_from, inputs.facts_to, inputs.anchor, inputs.authority, {}};
            for (const auto& bytes : inputs.body.facts) {
                auto fact{DecodeFact(bytes)};
                if (!fact.has_value()) return fact.error();
                batch.facts.push_back(std::move(*fact));
            }
            if (auto checked{chain::CheckBatch(batch, after, maximum)}; !checked.has_value()) return checked.error();
            return Ok(chain::PollReply::Facts(std::move(batch)));
        }
        Result<void> VerifyAnchor(const Anchor& candidate, const Anchor& last_certified) const override
        {
            if (!source.observed || source.observed->anchor != candidate) return Error{ErrorCode::NEED_DATA, "anchor lacks wrapper observation"};
            if (source.owner->Head()->Header().anchor != last_certified || candidate.height < last_certified.height ||
                (candidate.height == last_certified.height && candidate != last_certified)) {
                return Error{ErrorCode::VERIFY_FAILED, "observed anchor differs from current certified ancestry"};
            }
            return Ok();
        }
    };
    struct OperatorSink final : agreement::MessageSink {
        struct Frame { uint32_t to; Bytes bytes; bool durable_vote; bool durable_control; };
        const ProfileParams& profile;
        const size_t frame_limit;
        std::vector<Frame> frames;
        net::SeatMesh* mesh{nullptr};
        bool regtest_drop_commits{false};
        uint64_t regtest_dropped_commits{0};
        explicit OperatorSink(const ProfileParams& profile_in, size_t limit) : profile{profile_in}, frame_limit{limit} {}
        Result<void> Queue(uint32_t to, std::span<const unsigned char> message, bool durable_vote, bool durable_control = false)
        {
            if (to >= 4 || message.size() > agreement::MaxMessageBytes(profile)) return Error{ErrorCode::INVALID_INPUT, "outbound agreement frame exceeds bounds"};
            if (frames.size() >= frame_limit) return Error{ErrorCode::BUSY, "operator agreement frame queue full"};
            frames.push_back(Frame{to, Bytes{message.begin(), message.end()}, durable_vote, durable_control});
            return Ok();
        }
        Result<void> Send(uint32_t to, std::span<const unsigned char> message) override
        { return mesh ? mesh->Send(to, message) : Queue(to, message, false); }
        Result<void> SendDurableVote(uint32_t to, const agreement::OutboundVote& vote, const uint256& hash,
                                      std::span<const unsigned char> message) override
        {
            if (mesh && regtest_drop_commits && vote.Key().phase == storage::VotePhase::COMMIT) {
                ++regtest_dropped_commits; return Ok(); // Explicit local REGTEST packet-loss fault, never an undurable vote.
            }
            return mesh ? mesh->SendDurableVote(to, vote, hash, message) : Queue(to, message, true);
        }
        Result<void> SendDurableControl(uint32_t to, const agreement::OutboundControl& control,
                                         std::span<const unsigned char> message) override
        { return mesh ? mesh->SendDurableControl(to, control, message) : Queue(to, message, false, true); }
    };
    std::unique_ptr<ObservedAdapter> adapter;
    std::unique_ptr<OperatorSink> sink;
    agreement::PbftSafetyRules safety;
    std::unique_ptr<agreement::Agreement> pbft;

    explicit Impl(const UniValue& config)
    {
        Need(Text(config, "format") == "fm2-regtest-operator/1", "unsupported regtest operator format");
        const std::string mode{config.exists("agreement") ? Text(config, "agreement") : "direct-commit"};
        Need(mode == "direct-commit" || mode == "pbft-fixed-view" || mode == "pbft-view-change", "unknown regtest agreement mode");
        pbft_mode = mode != "direct-commit";
        pbft_view_change = mode == "pbft-view-change";
        const auto deployment{Hex(config, "deployment_hex", 65536)}; Need(!deployment.empty(), "deployment identity required");
        UniValue deployment_json;
        Need(deployment_json.read(std::string{deployment.begin(), deployment.end()}) && deployment_json.isObject(), "deployment identity must encode an exact JSON object");
        network_mode = deployment_json.exists("transport");
        nitro_lab_mode = config.exists("nitro_lab");
        Need(nitro_lab_mode == deployment_json.exists("nitro_lab"), "lab config and deployment binding must both be explicit");
        if (nitro_lab_mode) {
            Need(!network_mode && !pbft_mode, "owned Nitro lab uses its explicit direct four-worker fixture mode");
            const auto binding{ReadNitroLab(config["nitro_lab"])};
            Need(binding == ReadNitroLab(deployment_json["nitro_lab"]), "configured and deployed Nitro lab identity differ");
            profile = Must(NitroLabRegtestProfile(ChainType::REGTEST, binding));
            q = *profile.stable_fee_asset;
            markets = {{a, q, 1'000'000, 10'000}, {b, q, 1'000'000, 10'000}};
        } else if (pbft_view_change) profile = Must(network_mode ? StableQNetworkRegtestProfile(ChainType::REGTEST) : StableQViewRegtestProfile(ChainType::REGTEST));
        seat = Number<uint32_t>(config, "seat"); Need(seat < 4, "seat outside static four-seat roster");
        signer_instance = Number<uint64_t>(config, "signer_instance"); commit_instance = Number<uint64_t>(config, "commit_instance");
        Need(signer_instance != 0 && commit_instance != 0, "journal instances must be explicit and nonzero");
        if (pbft_view_change) {
            control_instance = Number<uint64_t>(config, "control_instance");
            Need(control_instance == uint64_t{300} + seat && control_instance != signer_instance && control_instance != commit_instance,
                 "view-change requires explicit distinct control_instance=300+seat");
        } else Need(!config.exists("control_instance"), "legacy modes do not use a control journal");
        const auto ikm{Hex(config, "ikm", 32, 32)};
        const auto own_key{bls::SecretKey::FromIKM(ikm)}; Need(own_key.has_value(), "invalid local IKM");
        Need(config["roster"].isArray() && config["roster"].size() == 4, "exactly four public registrations required");
        std::vector<SeatRegistration> registrations;
        for (const auto& registration : config["roster"].getValues()) {
            const auto pk{bls::PublicKey::Decode(Hex(registration, "public_key", bls::PUBKEY_SIZE, bls::PUBKEY_SIZE))};
            const auto pop{bls::Signature::Decode(Hex(registration, "pop", bls::SIGNATURE_SIZE, bls::SIGNATURE_SIZE))};
            Need(pk.has_value() && pop.has_value(), "malformed public registration");
            registrations.push_back(SeatRegistration{*pk, *pop});
        }
        seats = Must(SeatSet::FromPoP(registrations));
        Need(seats->Size() == 4 && seats->Quorum() == 3 && own_key->GetPublicKey() == seats->Keys()[seat].Key(), "local key does not match fixed quorum roster");
        Need(Text(deployment_json, "format") == (nitro_lab_mode ? "fm2-owned-nitro-lab-deployment/1" : "fm2-regtest-deployment/1") &&
             Number<uint64_t>(deployment_json, "chain_id") == (nitro_lab_mode ? NITRO_LAB_CHAIN_ID : uint64_t{31337}) &&
             Text(deployment_json, "profile_name") == profile.profile_id, "deployment must be the exact local regtest profile/chain");
        if (nitro_lab_mode) Need(deployment_json["mainnet_enabled"].isBool() && !deployment_json["mainnet_enabled"].get_bool(),
                                 "owned Nitro lab deployment must explicitly keep mainnet disabled");
        if (pbft_view_change) Need(Text(deployment_json, "agreement") == mode, "deployment does not opt into view-change mode");
        if (network_mode) {
            Need(pbft_view_change && Text(deployment_json["transport"], "format") == "fm2-seat-tcp/1",
                 "authenticated TCP requires the explicit view-bound REGTEST profile");
            network_manifest = deployment_json["transport"];
            network_key = ClientKey(config["network_identity"]);
            Need(network_manifest["identity_keys"].isArray() && network_manifest["identity_keys"].size() == 4,
                 "transport identities must match every fixed seat");
            UniValue own{UniValue::VOBJ}; own.pushKV("key", network_manifest["identity_keys"][seat]);
            const XOnlyPubKey own_transport_key{network_key->GetPubKey()};
            Need(Hex(own, "key", 32, 32) == Bytes{own_transport_key.begin(), own_transport_key.end()},
                 "private transport key differs from the pinned seat identity");
        } else Need(!config.exists("network_identity"), "legacy deployment cannot use a transport key");
        const UniValue& routes{deployment_json["routes"]};
        Need(routes.isObject() && routes.size() == 3, "deployment needs exactly Q/TOKENA/TOKENB custody routes");
        for (const auto& [name, asset] : std::vector<std::pair<std::string, AssetId>>{{"Q", q}, {"TOKENA", a}, {"TOKENB", b}}) {
            Need(AssetId{JsonHash(routes[name], "custody_asset")} == asset, "deployment custody asset differs from fixed market registry");
        }
        Need(deployment_json["roster"].isArray() && deployment_json["roster"].size() == 4, "deployment roster required");
        for (size_t index{0}; index < 4; ++index) {
            const auto& row{deployment_json["roster"][index]};
            Need(Hex(row, "public_key", bls::PUBKEY_SIZE, bls::PUBKEY_SIZE) ==
                     Hex(config["roster"][index], "public_key", bls::PUBKEY_SIZE, bls::PUBKEY_SIZE) &&
                 Hex(row, "pop", bls::SIGNATURE_SIZE, bls::SIGNATURE_SIZE) ==
                     Hex(config["roster"][index], "pop", bls::SIGNATURE_SIZE, bls::SIGNATURE_SIZE), "deployment public authority differs from configured roster");
        }
        const Anchor anchor{ReadAnchor(config["genesis_anchor"])};
        const FactCursor start{Cursor(config["facts_start"])};
        Need(start.height >= anchor.height && uint64_t{start.height} <= uint64_t{anchor.height} + 1, "genesis fact cursor outside anchor boundary");
        Bytes domain_bytes;
        VectorWriter{domain_bytes, 0, std::string{nitro_lab_mode ? "fm2-owned-nitro-lab/operator/v1" : "fm2-regtest/operator/v1"},
                     deployment, ProfileHash(profile), seats->Root(), markets, anchor, start};
        domain = Tagged(HashTag::NET_DOMAIN, domain_bytes);
        authority = AuthorityVersion{0, seats->Root(), ProfileHash(profile)};
        genesis = Must(state::GenesisHead(profile, markets, domain, authority, anchor, start));
        directory = fs::PathFromString(Text(config, "datadir"));
        Need(directory.is_absolute() && directory == directory.lexically_normal(), "datadir must be an explicit normal absolute path");
        Need(config["create"].isBool(), "create must be explicit true or false");
        UniValue identity{UniValue::VOBJ};
        identity.pushKV("format", "fm2-regtest-operator/1"); identity.pushKV("domain", HexStr(domain));
        identity.pushKV("seat", seat); identity.pushKV("public_key", HexStr(own_key->GetPublicKey().Compressed()));
        identity.pushKV("signer_instance", signer_instance); identity.pushKV("commit_instance", commit_instance);
        identity.pushKV("genesis", HexStr(genesis->Header().Encode()));
        if (pbft_mode) identity.pushKV("agreement", mode);
        if (pbft_view_change) {
            identity.pushKV("control_instance", control_instance);
            identity.pushKV("profile_id", profile.profile_id); identity.pushKV("profile_hash", HexStr(ProfileHash(profile)));
        }
        const std::string expected{identity.write()};
        const auto signer_dir{directory / "signer"}, commit_dir{directory / "commit"};
        const auto control_dir{directory / "control"};
        std::error_code ec;
        if (config["create"].get_bool()) {
            if (!std::filesystem::exists(directory, ec)) Need(fs::create_directory(directory, ec) && !ec, "cannot create explicit datadir");
            Need(fs::is_directory(fs::symlink_status(directory, ec)) && !ec && fs::is_empty(directory, ec) && !ec, "new datadir must be an empty real directory");
            Need(fs::create_directory(signer_dir, ec) && !ec && fs::create_directory(commit_dir, ec) && !ec, "journal directory creation failed");
            if (pbft_view_change) Need(fs::create_directory(control_dir, ec) && !ec, "control journal directory creation failed");
            CreateIdentity(directory / "identity.json", expected);
            signer_journal = Must(storage::FileJournal::CreateNew(signer_dir, storage::JournalRole::SIGNER, signer_instance, storage::DEFAULT_SEGMENT_BYTES));
            commit_journal = Must(storage::FileJournal::CreateNew(commit_dir, storage::JournalRole::COMMIT, commit_instance, storage::DEFAULT_SEGMENT_BYTES));
            if (pbft_view_change) control_journal = Must(storage::FileJournal::CreateNew(control_dir, storage::JournalRole::SIGNER, control_instance, storage::DEFAULT_SEGMENT_BYTES));
        } else {
            Need(fs::is_directory(fs::symlink_status(directory, ec)) && !ec, "original datadir missing");
            const auto actual{ReadIdentity(directory / "identity.json")};
            Need(std::string{actual.begin(), actual.end()} == expected, "original store identity differs; no replacement permitted");
            signer_journal = Must(storage::FileJournal::OpenExisting(signer_dir, storage::JournalRole::SIGNER, signer_instance));
            commit_journal = Must(storage::FileJournal::OpenExisting(commit_dir, storage::JournalRole::COMMIT, commit_instance));
            if (pbft_view_change) control_journal = Must(storage::FileJournal::OpenExisting(control_dir, storage::JournalRole::SIGNER, control_instance));
        }
        signer_log = std::make_unique<storage::SignerLog>(Must(storage::SignerLog::Create(*signer_journal, profile)));
        // Restore the refusal guard before exposing a fresh agreement object.
        // A rejected conflicting first message after restart must not install
        // a volatile proposal that blocks the actual durable obligation.
        const auto signer_records{Must(signer_journal->ReadAll())};
        for (const auto& record : signer_records) {
            if (record.type != storage::RecordType::ACCEPTED_PROPOSAL) continue;
            const auto accepted{Must(storage::AcceptedProposalRecord::Decode(record.payload, profile))};
            const auto [entry, inserted]{accepted_headers.emplace(accepted.key, accepted.header.Hash())};
            Need(inserted || entry->second == accepted.header.Hash(), "recovered accepted proposal conflict");
        }
        commit_log = std::make_unique<storage::CommitLog>(Must(storage::CommitLog::Create(*commit_journal, profile)));
        if (pbft_view_change) {
            const storage::ControlConfig control_config{domain, ProfileHash(profile), seats->Root(), 0, seat, 4, seats->Keys()[seat].Key()};
            // Create authenticates and reconstructs the existing immutable
            // control history too. Missing/corrupt history is never replaced.
            control_log = std::make_unique<storage::ControlLog>(Must(storage::ControlLog::Create(*control_journal, control_config)));
            signer = std::make_unique<storage::SeatSigner>(Must(storage::SeatSigner::FromIKM(ikm, *signer_log, *control_log)));
        } else signer = std::make_unique<storage::SeatSigner>(Must(storage::SeatSigner::FromIKM(ikm, *signer_log)));
        owner = Must(state::StateOwner::Create(profile, *genesis, commit_instance));
        const auto records{Must(commit_journal->ReadAll())};
        const auto recovered{Must(owner->RestoreVerifiedGenesis(*genesis, records, *seats))};
        (void)recovered;
        read = Must(read::ReadService::Create(read::ReadLimits{8, read::CertifiedEnvelope::MAX_ENCODED_BYTES, 8, profile.max_delay_slots}, profile));
        pool = Must(admission::ActionPool::Create(admission::PoolLimits::FromProfile(profile), profile, 0));
        // Rehydrate certified read/pool indexes by actual reexecution and real
        // receipts. A decided-but-unapplied tail completes all four barriers
        // before any API is created or open acknowledgement is returned.
        auto replay_owner{Must(state::StateOwner::Create(profile, *genesis, commit_instance))};
        for (const auto& record : records) {
            if (record.type != storage::RecordType::DECISION) continue;
            auto decision{Must(storage::DecisionRecord::Decode(record.payload, profile))};
            if (decisions.contains(decision.header.height)) continue;
            auto candidate{Must(replay_owner->Reexecute(decision.header, decision.body))};
            auto outcomes{candidate.Outcomes()};
            const auto pending{Must(commit_log->AppendDecision(decision.header, decision.certificate, decision.body))};
            const auto token{Must(commit_log->Confirm(pending))};
            const auto installed{Must(replay_owner->Apply(std::move(candidate), token, authority))};
            const auto applied{Must(commit_log->AppendApplied(installed.receipt))};
            Must(commit_log->Barrier());
            const auto durable{Must(commit_log->Confirm(applied))};
            Must(read->Publish(durable, Must(read::SlotView::Build(*installed.head, decision.certificate, outcomes))));
            Must(pool->OnCommitted(durable, *installed.head, outcomes, decision.body));
            certificates.emplace(decision.header.height, decision.certificate);
            decisions.emplace(decision.header.height, std::move(decision));
        }
        owner = std::move(replay_owner);
        if (config.exists("observer_handover")) RestoreObservationAuthority(config["observer_handover"]);
        // A recovered vote/QC fixes the source batch, not the actions that a
        // later valid NEW_VIEW may select. Keep it unauthorised until the
        // wrapper independently rechecks the original B3/EVM observation.
        const uint64_t next_slot{owner->Head()->Header().height + 1};
        for (const auto& record : signer_records) {
            if (record.type != storage::RecordType::ACCEPTED_PROPOSAL) continue;
            const auto accepted{Must(storage::AcceptedProposalRecord::Decode(record.payload, profile))};
            if (accepted.header.height == next_slot && !SupersededObservation(accepted.header, accepted.body, accepted.key.view)) {
                RememberObservation(accepted.header, accepted.body);
            }
        }
        for (const auto& record : records) {
            if (record.type != storage::RecordType::PREPARED_QC) continue;
            const auto prepared{Must(storage::PreparedQcRecord::Decode(record.payload, profile))};
            if (prepared.header.height == next_slot && !SupersededObservation(prepared.header, prepared.body, prepared.view)) {
                RememberObservation(prepared.header, prepared.body);
            }
        }
        budget = Must(UnlimitedDevBudget::Create(profile));
        admission = Must(admission::Admission::Create(profile, *owner, *read, *pool, *budget));
        if (pbft_mode) {
            adapter = std::make_unique<ObservedAdapter>(*this);
            sink = std::make_unique<OperatorSink>(profile, pbft_view_change ? 64 : 24);
            agreement::PbftConfig agreement_config;
            agreement_config.self_seat = seat; agreement_config.regtest_fixed_view = !pbft_view_change;
            agreement_config.regtest_full_view = pbft_view_change;
            if (network_mode) agreement_config.view_timeout_floor_ms = 3000;
            if (owner->Head()->Header().height) agreement_config.parent_certificate = certificates.at(owner->Head()->Header().height);
            pbft = Must(agreement::PbftAgreement::Create(profile, agreement_config,
                agreement::PbftContext{*adapter, *owner, *signer, *signer_log, *commit_log, safety, *sink, control_log.get()}));
            ++network_controller_generation;
            if (pbft_view_change) {
                const auto control{control_log->ViewAt(next_slot)};
                if (control && control->accepted) {
                    const auto message{Must(agreement::DecodeMessage(control->accepted->message, profile))};
                    const auto* view{std::get_if<agreement::ControlNewView>(&message)};
                    Need(view != nullptr, "recovered accepted control view has another message type");
                    if (view->selected_proposal && !SupersededObservation(view->selected_proposal->header,
                            view->selected_proposal->body, view->key.view)) {
                        RememberObservation(view->selected_proposal->header, view->selected_proposal->body);
                    }
                }
            }
            if (observation_authority) FenceObservationAuthority();
        }
    }

    net::TcpMeshConfig NetworkConfig() const
    {
        Need(network_mode && network_key.has_value(), "deployment has no authenticated transport identity");
        net::TcpMeshConfig config;
        config.mesh = net::MeshConfig{seat, 4, 0x00fff000, profile};
        config.domain = domain; config.seat_set_hash = seats->Root(); config.epoch = 0;
        const auto magic{Hex(network_manifest, "network_magic", 4, 4)};
        std::copy(magic.begin(), magic.end(), config.network_magic.begin());
        Need(network_manifest["endpoints"].isArray() && network_manifest["endpoints"].size() == 4, "four pinned endpoints required");
        for (uint32_t i{0}; i < 4; ++i) {
            UniValue holder{UniValue::VOBJ}; holder.pushKV("key", network_manifest["identity_keys"][i]);
            const auto bytes{Hex(holder, "key", 32, 32)}; std::array<unsigned char, 32> key{};
            std::copy(bytes.begin(), bytes.end(), key.begin()); config.identity_keys.push_back(key);
            const auto& row{network_manifest["endpoints"][i]};
            Need(Number<uint32_t>(row, "seat") == i && Number<uint64_t>(row, "epoch") == 0 && Number<uint64_t>(row, "counter") > 0,
                 "endpoint seat/epoch/counter differs from its pinned manifest");
            net::SeatEndpoint endpoint; endpoint.seat = i; endpoint.valid_from_epoch = 0;
            endpoint.counter = Number<uint64_t>(row, "counter"); endpoint.port = Number<uint16_t>(row, "port");
            // The qualification runtime has no non-loopback/mainnet activation path.
            Need(Text(row, "address") == "127.0.0.1", "regtest runtime endpoints must be literal loopback");
            endpoint.addresses.push_back("127.0.0.1"); config.endpoints.push_back(endpoint);
            config.minimum_endpoint_counters.push_back(endpoint.counter);
            if (i == seat) config.listen_port = endpoint.port;
        }
        config.sign_session = [this](const uint256& digest) -> Result<std::array<unsigned char, 64>> {
            std::array<unsigned char, 64> signature{};
            if (!network_key->SignSchnorr(digest, signature, nullptr, uint256{})) return Error{ErrorCode::VERIFY_FAILED, "transport session signing failed"};
            return Ok(signature);
        };
        return config;
    }
    UniValue NetworkEndpoint()
    {
        const auto config{NetworkConfig()};
        const auto digest{net::TcpSeatMesh::EndpointDigest(config, config.endpoints[seat])};
        const auto signature{Must(config.sign_session(digest))};
        UniValue out{UniValue::VOBJ}; out.pushKV("seat", seat); out.pushKV("signature", HexStr(signature)); return out;
    }
    UniValue NetworkStart(const UniValue& request)
    {
        Need(!mesh && network_mode, "authenticated network is unavailable or already started");
        auto config{NetworkConfig()};
        Need(request["signatures"].isArray() && request["signatures"].size() == 4, "four endpoint signatures required");
        for (uint32_t i{0}; i < 4; ++i) {
            UniValue holder{UniValue::VOBJ}; holder.pushKV("signature", request["signatures"][i]);
            const auto signature{Hex(holder, "signature", 64, 64)};
            std::copy(signature.begin(), signature.end(), config.endpoints[i].identity_signature.begin());
        }
        mesh = Must(net::TcpSeatMesh::Create(config)); sink->mesh = mesh.get(); return Info();
    }
    UniValue NetworkPoll()
    {
        Need(bool(mesh), "start the authenticated network first");
        UniValue out{UniValue::VOBJ};
        size_t pending_bytes{0};
        for (const auto& [token, pending] : network_pending) { (void)token; pending_bytes += pending.bytes.size(); }
        // Poll must keep progressing handshakes and outgoing retries even
        // while this observer queue is full. Application delivery has no ACK
        // here: a bounded dropped frame is recovered by PBFT/history retries.
        const auto frame{Must(mesh->Poll())};
        if (!frame || network_pending.size() >= NETWORK_PENDING_LIMIT ||
            pending_bytes + frame->bytes.size() > 32 * 1024 * 1024) {
            out.pushKV("received", false); return out;
        }
        const uint64_t token{++next_network_token};
        Need(token != 0, "private network receive token exhausted");
        network_pending.emplace(token, *frame);
        out.pushKV("received", true); out.pushKV("token", token); out.pushKV("from_seat", frame->from_seat);
        out.pushKV("lane", static_cast<uint8_t>(frame->lane)); out.pushKV("message", HexStr(frame->bytes));
        if (frame->lane == net::Lane::CRITICAL || frame->lane == net::Lane::PROPOSAL) {
            const auto decoded{agreement::DecodeMessage(frame->bytes, profile)};
            if (decoded.has_value()) {
                const agreement::Proposal* proposal{nullptr};
                if (const auto* value{std::get_if<agreement::Proposal>(&*decoded)}) proposal = value;
                else if (const auto* value{std::get_if<agreement::OfferedProposal>(&*decoded)}) proposal = &value->proposal;
                else if (const auto* value{std::get_if<agreement::CertifiedProposal>(&*decoded)}) proposal = &value->proposal;
                else if (const auto* value{std::get_if<agreement::ControlNewView>(&*decoded)}; value && value->selected_proposal) proposal = &*value->selected_proposal;
                if (std::holds_alternative<agreement::ControlNewView>(*decoded)) out.pushKV("is_new_view", true);
                if (std::holds_alternative<agreement::OfferedProposal>(*decoded)) out.pushKV("is_offer", true);
                if (proposal) { out.pushKV("header", HexStr(proposal->header.Encode())); out.pushKV("slot", proposal->header.height); }
                else if (const auto* value{std::get_if<agreement::Vote>(&*decoded)}) out.pushKV("slot", value->slot);
                else if (const auto* value{std::get_if<agreement::ControlReport>(&*decoded)}) out.pushKV("slot", value->key.slot);
                else if (const auto* value{std::get_if<agreement::ControlNewView>(&*decoded)}) out.pushKV("slot", value->key.slot);
            }
        }
        return out;
    }
    UniValue NetworkSend(const UniValue& request)
    {
        Need(bool(mesh), "start the authenticated network first");
        const auto bytes{Hex(request, "message", 0x00fff000)};
        Must(mesh->Send(Number<uint32_t>(request, "to_seat"), net::Lane::BULK, bytes));
        UniValue out{UniValue::VOBJ}; out.pushKV("queued", true); return out;
    }
    ObservationAuthority VerifyObservationAuthority(const Bytes& bytes, uint64_t token, bool restoring, bool own_quorum = false) const
    {
        Need(network_mode && pbft_view_change && control_log, "source handover requires the authenticated view-bound deployment");
        const auto message{Must(agreement::DecodeMessage(bytes, profile))};
        const auto* proof{std::get_if<agreement::ControlNewView>(&message)};
        Need(proof && proof->selected_proposal, "source handover requires an exact complete signed NEW_VIEW");
        Need(proof->key.slot == owner->Head()->Header().height + 1, "source handover is not the next committed-parent instance");
        own_quorum = own_quorum || (restoring && proof->key.seat == seat &&
            std::all_of(proof->signature.begin(), proof->signature.end(), [](auto b) { return b == 0; }));
        if (own_quorum) {
            Need(proof->key.seat == seat, "unsigned quorum handover is only this operator's actual leader bundle");
            Must(agreement::VerifyControlNewViewEvidence(*proof, owner->Head()->Header(), *seats, profile));
            CheckOwnHandoverReport(*proof);
            if (const auto intent{control_log->IntentAt(proof->key)}) Need(*intent == Must(agreement::ControlIntent(*proof, profile)),
                "own quorum handover conflicts with the immutable leader intent");
        } else Must(agreement::VerifyControlNewView(*proof, owner->Head()->Header(), *seats, profile));
        const auto& selected{*proof->selected_proposal};
        if (selected.parent_cert) CheckCertificate(owner->Head()->Header(), *selected.parent_cert);
        else Need(owner->Head()->Header().height == 0, "source handover omitted its certified parent");
        const auto execution{Must(owner->Reexecute(selected.header, selected.body))}; (void)execution;
        const auto known{control_log->AcceptedAt(proof->key.slot, proof->key.view)};
        if (known) {
            const auto accepted_message{Must(agreement::DecodeMessage(known->message, profile))};
            const auto* accepted{std::get_if<agreement::ControlNewView>(&accepted_message)};
            Need(accepted && known->key == proof->key, "source handover has another accepted control instance");
            Must(agreement::VerifyControlNewView(*accepted, owner->Head()->Header(), *seats, profile));
            Need(own_quorum ? Must(agreement::ControlIntent(*accepted, profile)) == Must(agreement::ControlIntent(*proof, profile)) : known->message == bytes,
                "source handover conflicts with immutable accepted NEW_VIEW");
        }
        const auto current{control_log->ViewAt(proof->key.slot)};
        Need(!current || proof->key.view >= current->view || (restoring && known.has_value()),
             "source handover would regress the durable control view");
        for (const auto view : signer_log->DurableAcceptedViewsAt(0, proof->key.slot)) {
            if (view < proof->key.view) continue;
            const auto accepted{Must(signer_log->AcceptedAt({0, proof->key.slot, view}, profile))};
            Need(!accepted || (restoring ? SameObservation(ObservationOf(accepted->header, accepted->body),
                    ObservationOf(selected.header, selected.body)) : accepted->header.Hash() == selected.header.Hash()),
                 "source handover conflicts with a same-or-higher durable accepted proposal");
        }
        if (const auto highest{commit_log->HighestDurablePreparedViewAt(0, proof->key.slot)}; highest && *highest >= proof->key.view) {
            const auto prepared{commit_log->PreparedAt(0, proof->key.slot, *highest)};
            Need(prepared && (restoring ? SameObservation(ObservationOf(prepared->header, prepared->body),
                    ObservationOf(selected.header, selected.body)) : prepared->header.Hash() == selected.header.Hash()),
                 "source handover conflicts with a same-or-higher durable prepared certificate");
        }
        return ObservationAuthority{token, *proof, bytes, restoring, false, own_quorum, std::nullopt};
    }
    void CheckOwnHandoverReport(const agreement::ControlNewView& proof, const SlotHeaderV2* retained_parent = nullptr) const
    {
        auto key{proof.key}; key.seat = seat; key.phase = storage::ControlPhase::VIEW_CHANGE;
        const auto intent{control_log->IntentAt(key)};
        Need(intent.has_value(), "own quorum handover lacks this operator's durable view-entry report");
        const auto message{Must(agreement::DecodeMessage(intent->message, profile))};
        const auto* report{std::get_if<agreement::ControlReport>(&message)};
        Need(report && report->key == key && report->prepared_watermark <= commit_log->DurableSeq(),
             "own quorum handover report differs from its durable prepared prefix");
        Must(agreement::VerifyControlReportEvidence(*report, retained_parent ? *retained_parent : owner->Head()->Header(), *seats, profile));
        const auto expected{commit_log->HighestPreparedAt(0, key.slot, key.view, report->prepared_watermark)};
        Need(expected.has_value() == report->prepared.has_value(), "own handover report omitted or invented its highest prepared certificate");
        if (expected) Need(report->prepared->view == expected->view && report->prepared->header == expected->header &&
             report->prepared->certificate.Encode() == expected->qc, "own handover report changed its immutable highest preparation");
        for (const auto& included : proof.reports) if (included.key.seat == seat) {
            Need(Must(agreement::ControlIntent(included, profile)) == *intent,
                 "own quorum proof contains another transcript for this operator's immutable report");
        }
    }
    agreement::CertifiedProposal DecodeCertifiedHistory(const Bytes& bytes) const
    {
        UniValue envelope;
        Need(envelope.read(std::string{bytes.begin(), bytes.end()}) && envelope.isObject() &&
             Text(envelope, "format") == "fm2-network-app/1" && Text(envelope, "kind") == "history_response",
             "source handover requires the exact authenticated certified history response");
        const auto& record{envelope["record"]};
        const auto header{Must(SlotHeaderV2::Decode(Hex(record, "header", SLOT_HEADER_BYTES, SLOT_HEADER_BYTES)))};
        const auto body{Must(SlotBody::Decode(Hex(record, "body", SlotBody::MaxEncodedSize(profile)), profile))};
        const auto certificate{Must(SlotCertificate::Decode(Hex(record, "certificate", SlotCertificate::MAX_ENCODED_BYTES)))};
        CheckCertificate(header, certificate);
        Need(header.height > 0 && certificate.view.has_value() && storage::BodyMatchesHeader(header, body),
             "certified history source proof differs from its exact view-bound body/header");
        return agreement::CertifiedProposal{agreement::Proposal{header.authority_epoch, *certificate.view, header, std::nullopt, body}, certificate};
    }
    ObservationAuthority VerifyHistoryObservationAuthority(const Bytes& bytes, uint64_t token, bool restoring) const
    {
        Need(network_mode && pbft_view_change, "certified source handover requires the authenticated view-bound deployment");
        auto final{DecodeCertifiedHistory(bytes)};
        const auto& header{final.proposal.header};
        Need(header.height == owner->Head()->Header().height + 1 && header.parent_hash == owner->Head()->HeaderHash(),
             "certified source handover is not the exact next committed-parent instance");
        if (const auto known{commit_log->DecisionAt(header.height)}) Need(known->header_hash == header.Hash(),
            "certified source handover conflicts with the retained local decision");
        if (observation_authority && observation_authority->history) Need(
            observation_authority->Selected().header == header && observation_authority->Selected().body == final.proposal.body,
            "certified source handover conflicts with the in-progress terminal decision");
        const auto reexecution{Must(owner->Reexecute(header, final.proposal.body))}; (void)reexecution;
        // A terminal q3 COMMIT certificate outranks unprepared later-view
        // observations. It authorizes this closed slot's source/apply only;
        // it never fabricates a view-change intent or grants a fresh vote.
        ObservationAuthority authorized;
        authorized.token = token; authorized.message = bytes; authorized.restored = restoring;
        authorized.history = std::move(final);
        return authorized;
    }
    void RestoreObservationAuthority(const UniValue& record)
    {
        Need(network_mode && record.isObject() && (record.size() == 1 || (record.size() == 2 && record.exists("authority_kind"))),
             "source handover recovery accepts only its exact saved proof and kind");
        const std::string kind{record.exists("authority_kind") ? Text(record, "authority_kind") : "signed-new-view"};
        Need(kind == "signed-new-view" || kind == "own-quorum" || kind == "certified-history", "unknown source handover authority kind");
        const auto bytes{Hex(record, "message", kind == "certified-history" ? 0x00fff000 : agreement::MaxMessageBytes(profile))};
        if (kind == "certified-history") {
            const auto final{DecodeCertifiedHistory(bytes)};
            const auto& header{final.proposal.header};
            if (header.height <= owner->Head()->Header().height) {
                const auto retained{decisions.find(header.height)};
                Need(retained != decisions.end() && retained->second.header == header && retained->second.body == final.proposal.body,
                     "saved certified source handover conflicts with the replayed retained decision");
                return; // Its exact terminal decision already passed verified replay.
            }
            observation_authority = VerifyHistoryObservationAuthority(bytes, 0, true);
            recovered_observation = ObservationOf(header, final.proposal.body);
            return;
        }
        const auto message{Must(agreement::DecodeMessage(bytes, profile))};
        const auto* proof{std::get_if<agreement::ControlNewView>(&message)};
        Need(proof && proof->selected_proposal && proof->key.slot > 0, "saved source handover is not a complete NEW_VIEW");
        if (proof->key.slot <= owner->Head()->Header().height) {
            const SlotHeaderV2* parent{nullptr};
            if (proof->key.slot == 1) parent = &genesis->Header();
            else if (const auto found{decisions.find(proof->key.slot - 1)}; found != decisions.end()) parent = &found->second.header;
            Need(parent != nullptr, "saved source handover lacks its retained certified parent");
            if (proof->key.seat == seat && std::all_of(proof->signature.begin(), proof->signature.end(), [](auto b) { return b == 0; })) {
                Must(agreement::VerifyControlNewViewEvidence(*proof, *parent, *seats, profile));
                CheckOwnHandoverReport(*proof, parent);
                const auto intent{control_log->IntentAt(proof->key)};
                const auto signature{control_log->SignedAt(proof->key)};
                if (intent) Need(*intent == Must(agreement::ControlIntent(*proof, profile)),
                    "applied own handover conflicts with an existing immutable leader transcript");
                if (signature) {
                    Need(intent.has_value(), "applied own handover signature lacks its original durable intent");
                    auto authenticated{*proof}; authenticated.signature = Must(control_log->Confirm(*signature)).Binding().signature;
                    Must(agreement::VerifyControlNewView(authenticated, *parent, *seats, profile));
                }
            } else Must(agreement::VerifyControlNewView(*proof, *parent, *seats, profile));
            const auto found{decisions.find(proof->key.slot)};
            Need(found != decisions.end() && SameObservation(ObservationOf(found->second.header, found->second.body),
                    ObservationOf(proof->selected_proposal->header, proof->selected_proposal->body)),
                 "saved source handover differs from the exact source of the certified retained prefix");
            return; // Already applied, never re-arm an old observation capability.
        }
        observation_authority = VerifyObservationAuthority(bytes, 0, true);
        recovered_observation = ObservationOf(proof->selected_proposal->header, proof->selected_proposal->body);
    }
    UniValue NetworkObservationAuthority(const UniValue& request)
    {
        Need(!observation_authority || !observation_authority->history, "terminal certified apply must finish before another view authority");
        const uint64_t token{Number<uint64_t>(request, "token")};
        const auto found{network_pending.find(token)};
        Need(found != network_pending.end() && found->second.lane == net::Lane::CRITICAL,
             "source handover requires an unconsumed authenticated critical token");
        auto authorized{VerifyObservationAuthority(found->second.bytes, token, false)};
        Need(found->second.from_seat == authorized.proof.key.seat, "source handover NEW_VIEW sender differs from its proved session seat");
        // A read-only proof check can pause this single owner, but cannot
        // fabricate a control record or authorize any ordinary signature.
        observation_authority = std::move(authorized);
        return ObservationAuthorityJson();
    }
    UniValue ObservationAuthorityJson() const
    {
        const auto& selected{observation_authority->Selected()};
        UniValue out{UniValue::VOBJ}, actions{UniValue::VARR}, facts{UniValue::VARR};
        for (const auto& value : selected.body.originals) actions.push_back(HexStr(value));
        for (const auto& value : selected.body.facts) facts.push_back(HexStr(value));
        out.pushKV("available", true); out.pushKV("token", observation_authority->token); out.pushKV("view", observation_authority->View()); out.pushKV("slot", selected.header.height);
        out.pushKV("authority_kind", observation_authority->history ? "certified-history" : observation_authority->own_quorum ? "own-quorum" : "signed-new-view");
        out.pushKV("header", HexStr(selected.header.Encode()));
        if (observation_authority->history) {
            // The caller already retains this exact native receive frame.
            // Repeating its JSON hex alongside decoded actions/facts could
            // exceed the bounded worker reply for a maximum-size body.
            std::array<unsigned char, CSHA256::OUTPUT_SIZE> digest;
            CSHA256{}.Write(observation_authority->message.data(), observation_authority->message.size()).Finalize(digest.data());
            out.pushKV("message_sha256", HexStr(digest));
        } else out.pushKV("message", HexStr(observation_authority->message));
        out.pushKV("actions", actions); out.pushKV("facts", facts);
        return out;
    }
    UniValue NetworkOwnObservationAuthority(const UniValue& request)
    {
        Need(!observation_authority || !observation_authority->history, "terminal certified apply must finish before another leader authority");
        Need(network_mode && pbft && pbft_view_change, "own quorum handover requires authenticated full-view agreement");
        std::optional<agreement::Proposal> candidate;
        if (request.exists("token")) {
            const auto found{network_pending.find(Number<uint64_t>(request, "token"))};
            Need(found != network_pending.end() && found->second.lane == net::Lane::PROPOSAL, "own handover body requires a stored authenticated offer token");
            const auto message{Must(agreement::DecodeMessage(found->second.bytes, profile))};
            const auto* offer{std::get_if<agreement::OfferedProposal>(&message)};
            Need(offer != nullptr, "own quorum availability token is not an offered body"); candidate = offer->proposal;
        }
        const auto pending{Must(pbft->PendingObservationNewView(candidate))};
        if (!pending) { UniValue out{UniValue::VOBJ}; out.pushKV("available", false); return out; }
        const auto bytes{Must(agreement::EncodeMessage(agreement::Message{*pending}, profile))};
        const uint64_t token{++next_network_token}; Need(token != 0, "private observation capability exhausted");
        observation_authority = VerifyObservationAuthority(bytes, token, false, true);
        return ObservationAuthorityJson();
    }
    UniValue NetworkHistoryObservationAuthority(const UniValue& request)
    {
        const uint64_t token{Number<uint64_t>(request, "token")};
        const auto found{network_pending.find(token)};
        Need(found != network_pending.end() && found->second.lane == net::Lane::BULK,
             "certified source handover requires its unconsumed authenticated bulk token");
        observation_authority = VerifyHistoryObservationAuthority(found->second.bytes, token, false);
        return ObservationAuthorityJson();
    }
    void FenceObservationAuthority()
    {
        Need(observation_authority.has_value() && pbft, "source handover has no verified authority or controller");
        if (observation_authority->fenced) return;
        // Drive the normal controller with no newly trusted source. Its real
        // EnterView creates the truthful immutable own report and barrier;
        // the selected body waits for independent observer availability.
        observed.reset();
        if (observation_authority->history) {
            // The durable wrapper proof plus this single-owner pause gate
            // fences all signing until certified apply, including restart.
            // No control-log entry is invented for a completed lower view.
            observation_authority->fenced = true;
            return;
        }
        const auto proof{observation_authority->proof}; const auto bytes{observation_authority->message};
        if (!observation_authority->own_quorum) RunAgreement([&] { return pbft->OnMessage(proof.key.seat, bytes); });
        else CheckOwnHandoverReport(proof);
        const auto control{control_log->ViewAt(proof.key.slot)};
        Need(control && control->view >= proof.key.view, "source handover did not durably fence the abandoned view");
        observation_authority->fenced = true;
    }
    UniValue NetworkResumeObservation()
    {
        Need(observation_authority.has_value() && observed.has_value(), "source handover must first restore its independently checked observation");
        Need(SameObservation(*observed, ObservationOf(observation_authority->Selected().header,
                observation_authority->Selected().body)), "restored source differs from the exact selected authority");
        FenceObservationAuthority();
        if (observation_authority->history) {
            const auto final{*observation_authority->history};
            const uint64_t token{observation_authority->token};
            UniValue record{UniValue::VOBJ}; record.pushKV("header", HexStr(final.proposal.header.Encode()));
            record.pushKV("body", HexStr(final.proposal.body.Encode())); record.pushKV("certificate", HexStr(final.certificate.Encode()));
            const auto applied{ApplyHistoricalRecord(record)};
            if (token != 0) network_pending.erase(token);
            return applied;
        }
        const auto proof{observation_authority->proof}; const auto bytes{observation_authority->message};
        if (observation_authority->own_quorum) {
            if (pbft->CurrentView() <= proof.key.view) RunAgreement([&] { return pbft->ResumeObservationNewView(proof); });
        } else RunAgreement([&] { return pbft->OnMessage(proof.key.seat, bytes); });
        observation_authority.reset();
        return DrainAgreement();
    }
    UniValue NetworkCheckHistory(const UniValue& request) const
    {
        const auto found{network_pending.find(Number<uint64_t>(request, "token"))};
        Need(found != network_pending.end() && found->second.lane == net::Lane::BULK,
             "history preflight requires an unconsumed authenticated bulk token");
        UniValue envelope;
        Need(envelope.read(std::string{found->second.bytes.begin(), found->second.bytes.end()}) && envelope.isObject() &&
             Text(envelope, "format") == "fm2-network-app/1" && Text(envelope, "kind") == "history_response",
             "unknown authenticated history response format");
        const auto& record{envelope["record"]};
        const auto header{Must(SlotHeaderV2::Decode(Hex(record, "header", SLOT_HEADER_BYTES, SLOT_HEADER_BYTES)))};
        const auto body{Must(SlotBody::Decode(Hex(record, "body", SlotBody::MaxEncodedSize(profile)), profile))};
        const auto certificate{Must(SlotCertificate::Decode(Hex(record, "certificate", SlotCertificate::MAX_ENCODED_BYTES)))};
        CheckCertificate(header, certificate);
        Need(storage::BodyMatchesHeader(header, body), "authenticated historical body differs from certified header");
        if (header.height == owner->Head()->Header().height + 1) {
            Need(header.parent_hash == owner->Head()->HeaderHash(), "historical prefix has another committed parent");
            const auto candidate{Must(owner->Reexecute(header, body))}; (void)candidate;
        } else if (header.height <= owner->Head()->Header().height) {
            const auto existing{decisions.find(header.height)};
            Need(existing != decisions.end() && existing->second.header == header && existing->second.body == body,
                 "historical response conflicts with the retained certified prefix");
        }
        UniValue out{UniValue::VOBJ}, actions{UniValue::VARR}, facts{UniValue::VARR};
        for (const auto& value : body.originals) actions.push_back(HexStr(value));
        for (const auto& value : body.facts) facts.push_back(HexStr(value));
        out.pushKV("acceptable", true); out.pushKV("height", header.height); out.pushKV("header", HexStr(header.Encode()));
        out.pushKV("actions", actions); out.pushKV("facts", facts); return out;
    }
    UniValue ApplyHistoricalRecord(const UniValue& record)
    {
        const auto applied{Commit(record)}; // Real q3 DECISION/APPLIED, reexecution and publication.
        if (applied["duplicate"].get_bool()) {
            // Retained retransmissions cannot reset a live slot's votes/timer.
            UniValue out{UniValue::VOBJ}; out.pushKV("applied", UniValue{UniValue::VARR});
            out.pushKV("frames", UniValue{UniValue::VARR}); out.pushKV("info", Info()); return out;
        }
        critical = true; // A post-apply reconstruction error requires verified reopen.
        agreement::PbftConfig config; config.self_seat = seat; config.regtest_full_view = true; config.view_timeout_floor_ms = 3000;
        config.parent_certificate = certificates.at(owner->Head()->Header().height);
        pbft = Must(agreement::PbftAgreement::Create(profile, config,
            agreement::PbftContext{*adapter, *owner, *signer, *signer_log, *commit_log, safety, *sink, control_log.get()}));
        ++network_controller_generation;
        if (observation_authority && observation_authority->history) observation_authority.reset();
        UniValue item{UniValue::VOBJ}; item.pushKV("header", record["header"]); item.pushKV("body", record["body"]);
        item.pushKV("certificate", record["certificate"]); item.pushKV("head_hash", applied["head_hash"]);
        item.pushKV("height", applied["height"]); item.pushKV("outcomes", applied["outcomes"]);
        UniValue decisions_out{UniValue::VARR}; decisions_out.push_back(item);
        UniValue out{UniValue::VOBJ}; out.pushKV("applied", decisions_out); out.pushKV("frames", UniValue{UniValue::VARR}); out.pushKV("info", Info());
        critical = false;
        return out;
    }
    UniValue NetworkDispatch(const UniValue& request)
    {
        Need(bool(mesh), "start the authenticated network first");
        const auto found{network_pending.find(Number<uint64_t>(request, "token"))};
        Need(found != network_pending.end(), "network receive token is unknown or already consumed");
        if (observation_authority) {
            Need(observation_authority->token == Number<uint64_t>(request, "token") && observation_authority->fenced &&
                 observed && SameObservation(*observed, ObservationOf(observation_authority->Selected().header,
                    observation_authority->Selected().body)),
                 "source handover must durably fence and independently observe the exact selected authority before dispatch");
            Need(observation_authority->history ? found->second.lane == net::Lane::BULK : found->second.lane == net::Lane::CRITICAL,
                 "source handover capability cannot cross agreement/certified-history dispatch kinds");
        }
        const auto frame{found->second};
        if (frame.lane == net::Lane::BULK) {
            Need(request["apply_history"].isBool(), "bulk dispatch requires explicit history choice");
            Need(!observation_authority || request["apply_history"].get_bool(), "cannot discard an in-progress certified source authority");
            if (!request["apply_history"].get_bool()) {
                network_pending.erase(found); UniValue out{UniValue::VOBJ}; out.pushKV("consumed", true); return out;
            }
            UniValue envelope;
            Need(envelope.read(std::string{frame.bytes.begin(), frame.bytes.end()}) && envelope.isObject() &&
                 Text(envelope, "format") == "fm2-network-app/1" && Text(envelope, "kind") == "history_response",
                 "history apply requires the exact authenticated history response frame");
            const auto& record{envelope["record"]};
            const auto applied{ApplyHistoricalRecord(record)};
            network_pending.erase(found);
            return applied;
        }
        Need(frame.lane == net::Lane::CRITICAL || frame.lane == net::Lane::PROPOSAL, "unknown application lane cannot enter agreement");
        UniValue command{UniValue::VOBJ}; command.pushKV("from_seat", frame.from_seat); command.pushKV("message", HexStr(frame.bytes));
        const auto result{PbftReceive(command)};
        if (observation_authority && observation_authority->token == Number<uint64_t>(request, "token")) observation_authority.reset();
        network_pending.erase(found); return result;
    }
    UniValue NetworkHistory(const UniValue& request) const
    {
        Need(network_mode, "history export requires authenticated network deployment");
        const auto height{Number<uint64_t>(request, "height")};
        const auto found{decisions.find(height)};
        UniValue out{UniValue::VOBJ}; out.pushKV("found", found != decisions.end());
        if (found != decisions.end()) {
            out.pushKV("header", HexStr(found->second.header.Encode())); out.pushKV("body", HexStr(found->second.body.Encode()));
            out.pushKV("certificate", HexStr(found->second.certificate.Encode()));
        }
        return out;
    }

    UniValue Info() const
    {
        const auto head{owner->Head()};
        UniValue out{UniValue::VOBJ}, assets{UniValue::VOBJ}, descriptions{UniValue::VOBJ};
        out.pushKV("mode", pbft_view_change ? "regtest-pbft-view-change" : pbft_mode ? "regtest-pbft-fixed-view" : "regtest-static-roster-direct-commit"); out.pushKV("pbft_complete", false);
        out.pushKV("pbft_fixed_view", pbft_mode && !pbft_view_change); out.pushKV("pbft_view_change", pbft_view_change);
        out.pushKV("pbft_timed_out", pbft_timed_out);
        const uint64_t view{pbft ? pbft->CurrentView() : 0};
        out.pushKV("current_view", view); out.pushKV("changing_view", pbft ? pbft->ChangingView() : false);
        out.pushKV("observation_frozen", ObservationFrozen());
        out.pushKV("next_leader", static_cast<uint32_t>(((head->Header().height + 1) % 4 + view % 4) % 4));
        out.pushKV("transport", network_mode ? "authenticated-bip324-tcp" : "trusted-local-wrapper-pipes");
        out.pushKV("network_active", bool(mesh)); out.pushKV("mainnet_enabled", false);
        out.pushKV("network_controller_generation", network_controller_generation);
        out.pushKV("observation_handover_pending", observation_authority.has_value());
        if (observation_authority) {
            out.pushKV("observation_handover_view", observation_authority->View());
            out.pushKV("observation_handover_token", observation_authority->token);
            out.pushKV("observation_handover_own_quorum", observation_authority->own_quorum);
            out.pushKV("observation_handover_fenced", observation_authority->fenced);
            out.pushKV("observation_handover_selected", HexStr(observation_authority->Selected().header.Hash()));
        }
        const auto prepared_view{commit_log->HighestDurablePreparedViewAt(0, head->Header().height + 1)};
        if (prepared_view) out.pushKV("pending_prepared_view", *prepared_view); else out.pushKV("pending_prepared_view", UniValue{});
        out.pushKV("regtest_commit_loss", sink && sink->regtest_drop_commits);
        out.pushKV("regtest_dropped_commits", sink ? sink->regtest_dropped_commits : uint64_t{0});
        out.pushKV("seat", seat); out.pushKV("seats", 4); out.pushKV("quorum", 3);
        out.pushKV("domain", HexStr(domain)); out.pushKV("profile_hash", HexStr(ProfileHash(profile)));
        out.pushKV("height", head->Header().height); out.pushKV("head_hash", HexStr(head->HeaderHash()));
        out.pushKV("state_root", HexStr(head->Header().state_root)); out.pushKV("header", HexStr(head->Header().Encode()));
        if (head->Header().height) out.pushKV("certificate", HexStr(certificates.at(head->Header().height).Encode()));
        else out.pushKV("certificate", UniValue{});
        assets.pushKV("Q", HexStr(q.id)); assets.pushKV("TOKENA", HexStr(a.id)); assets.pushKV("TOKENB", HexStr(b.id));
        out.pushKV("assets", std::move(assets));
        for (size_t i{0}; i < markets.size(); ++i) {
            const auto& market{markets[i]}; UniValue item{UniValue::VOBJ};
            item.pushKV("id", HexStr(market.Id().id)); item.pushKV("base", HexStr(market.base.id)); item.pushKV("quote", HexStr(q.id));
            item.pushKV("base_atoms_per_lot", market.base_atoms_per_lot); item.pushKV("quote_atoms_per_tick", market.quote_atoms_per_tick);
            descriptions.pushKV(i == 0 ? "TOKENA_Q" : "TOKENB_Q", std::move(item));
        }
        out.pushKV("markets", std::move(descriptions)); out.pushKV("facts_to", CursorJson(head->Header().facts_to));
        // Report the source checkpoint from the reexecuted certified body,
        // never from the wrapper's mutable source-current JSON. A reopened
        // observer must bind its retained proof cursor to this exact marker.
        if (profile.source_range_commitment && head->Header().height) {
            const auto& body{decisions.at(head->Header().height).body};
            Need(!body.facts.empty(), "certified body lacks source checkpoint");
            const auto fact{Must(DecodeFact(body.facts.back()))};
            const auto* range{std::get_if<SourceRangeFact>(&fact)};
            Need(range && range->domain == domain && range->chain_id == profile.source_chain_id &&
                 range->confirmations == profile.source_confirmations,
                 "certified source checkpoint differs from deployment");
            UniValue point{UniValue::VOBJ};
            point.pushKV("height", range->to_height);
            point.pushKV("hash", "0x" + HexStr(range->to_hash));
            point.pushKV("minimum_confirmations", range->confirmations);
            out.pushKV("source_checkpoint", std::move(point));
        }
        out.pushKV("signer_seq", signer_journal->DurableSeq()); out.pushKV("commit_seq", commit_journal->DurableSeq());
        if (pbft_view_change) out.pushKV("control_seq", control_journal->DurableSeq());
        out.pushKV("profile_name", profile.profile_id);
        if (nitro_lab_mode) {
            out.pushKV("nitro_lab", NitroLabJson(*profile.nitro_lab)); out.pushKV("lab_only", true);
            out.pushKV("mainnet_enabled", false); out.pushKV("rollup_settlement_verified", false);
        }
        out.pushKV("pending_actions", pool->Size()); return out;
    }

    void CheckCertificate(const SlotHeaderV2& header, const SlotCertificate& cert) const
    {
        Need(header.domain == domain && header.profile_hash == ProfileHash(profile) && header.authority_epoch == 0 &&
             header.seat_set_root == seats->Root() && header.commitment_version == ExpectedCommitmentVersion(profile), "candidate differs from pinned deployment/profile/authority");
        Must(verify::VerifyCertificate(header, cert, *seats));
    }
    void CheckObserved(const SlotHeaderV2& header, const SlotBody& body) const
    {
        Need(observed.has_value(), "trusted observer must authorize the full next batch first");
        Need(header.anchor == observed->anchor && header.facts_from == observed->facts_from && header.facts_to == observed->facts_to &&
             body.facts == observed->body.facts, "proposal differs from independently observed full batch");
    }
    void RememberObservation(const SlotHeaderV2& header, const SlotBody& body)
    {
        Need(header.parent_hash == owner->Head()->HeaderHash() && header.domain == domain && header.Authority() == authority,
             "recovered observation differs from the configured committed parent");
        auto inputs{ObservationOf(header, body)};
        if (recovered_observation) Need(SameObservation(*recovered_observation, inputs), "recovered durable obligations contain conflicting source batches");
        else recovered_observation = std::move(inputs);
    }
    state::SlotInputs ObservationOf(const SlotHeaderV2& header, const SlotBody& body) const
    {
        state::SlotInputs inputs;
        inputs.authority = authority; inputs.anchor = header.anchor; inputs.facts_from = header.facts_from;
        inputs.facts_to = header.facts_to; inputs.body.facts = body.facts;
        return inputs;
    }
    bool SupersededObservation(const SlotHeaderV2& header, const SlotBody& body, uint64_t view) const
    {
        Need(header.parent_hash == owner->Head()->HeaderHash() && header.domain == domain && header.Authority() == authority &&
             storage::BodyMatchesHeader(header, body), "recovered source obligation differs from its configured parent/authority/body");
        return observation_authority && (observation_authority->history || view < observation_authority->View()) &&
            !SameObservation(ObservationOf(header, body), ObservationOf(observation_authority->Selected().header,
                observation_authority->Selected().body));
    }
    static bool SameObservation(const state::SlotInputs& left, const state::SlotInputs& right)
    {
        return left.anchor == right.anchor && left.facts_from == right.facts_from && left.facts_to == right.facts_to &&
               left.body.facts == right.body.facts;
    }
    bool ObservationFrozen() const
    {
        if (batch_locked || recovered_observation.has_value()) return true;
        const uint64_t next_slot{owner->Head()->Header().height + 1};
        if (std::any_of(accepted_headers.begin(), accepted_headers.end(), [&](const auto& item) {
                return item.first.epoch == authority.epoch && item.first.slot == next_slot;
            })) return true;
        return control_log && control_log->ViewAt(next_slot).has_value();
    }
    state::SlotInputs ObservationInput(const UniValue& request) const
    {
        state::SlotInputs inputs;
        inputs.authority = authority; inputs.anchor = ReadAnchor(request["anchor"]);
        inputs.facts_from = Cursor(request["facts_from"]); inputs.facts_to = Cursor(request["facts_to"]);
        inputs.body.facts = Frames(request["facts"], MAX_SLOT_FACTS, MAX_FACT_BYTES);
        const auto head{owner->Head()};
        Need(inputs.facts_from == head->Header().facts_to, "observer batch is not contiguous with committed head");
        for (const auto& bytes : inputs.body.facts) {
            const Fact fact{Must(DecodeFact(bytes))};
            if (const auto* deposit{std::get_if<DepositFact>(&fact)}) Need(deposit->asset == a || deposit->asset == b || deposit->asset == q, "deposit route is not in deployment asset registry");
            else if (const auto* payout{std::get_if<PayoutConfirmedFact>(&fact)}) Need(payout->asset == q && payout->domain == domain, "payout differs from deployment Q/domain");
            else if (const auto* range{std::get_if<SourceRangeFact>(&fact)}) Need((network_mode || nitro_lab_mode) && profile.source_range_commitment && range->domain == domain &&
                range->chain_id == profile.source_chain_id && range->confirmations == profile.source_confirmations,
                "source-range marker differs from the authenticated network profile");
            else Fail(ErrorCode::UNSUPPORTED, "observer supplied an unsupported fact kind");
        }
        if (observation_authority) {
            if (!observation_authority->restored || request.exists("observation_token")) {
                Need(request.exists("observation_token") && Number<uint64_t>(request, "observation_token") == observation_authority->token &&
                     observation_authority->token != 0, "source handover requires its exact authenticated receive capability");
                if (!observation_authority->own_quorum) {
                    const auto found{network_pending.find(observation_authority->token)};
                    Need(found != network_pending.end() && found->second.bytes == observation_authority->message,
                         "source handover receive capability was consumed or changed");
                }
            }
            Need(SameObservation(inputs, ObservationOf(observation_authority->Selected().header,
                    observation_authority->Selected().body)),
                 "source handover observation differs from the quorum-selected exact facts/anchor/cursors");
            return inputs;
        }
        Need(!request.exists("observation_token"), "observer capability is unknown or already consumed");
        if (ObservationFrozen()) {
            const auto* expected{observed ? &*observed : recovered_observation ? &*recovered_observation : nullptr};
            // A report with no prepared value binds no facts. Its private
            // wrapper must still restore the exact saved source checkpoint;
            // once restored, this native guard fixes that batch as well.
            if (expected) Need(SameObservation(*expected, inputs), "cannot replace the source batch of a durable obligation");
        }
        return inputs;
    }
    UniValue CheckObservation(const UniValue& request) const
    {
        (void)ObservationInput(request);
        UniValue out{UniValue::VOBJ}; out.pushKV("acceptable", true); out.pushKV("observation_frozen", ObservationFrozen()); return out;
    }
    UniValue Observe(const UniValue& request)
    {
        auto inputs{ObservationInput(request)};
        if (observation_authority) {
            FenceObservationAuthority();
            recovered_observation = ObservationOf(observation_authority->Selected().header,
                observation_authority->Selected().body);
            batch_locked = true;
        }
        observed = std::move(inputs);
        UniValue out{UniValue::VOBJ}; out.pushKV("facts", observed->body.facts.size()); return out;
    }
    UniValue Candidate(const UniValue& request)
    {
        Need(observed.has_value(), "trusted observed batch required");
        auto inputs{*observed};
        std::optional<admission::TakenSlot> taken;
        if (request.exists("actions")) inputs.body.originals = Frames(request["actions"], profile.max_actions_per_slot, ActionV2::MaxEncodedSize(profile));
        else {
            const size_t maximum{request.exists("max_actions") ? Number<size_t>(request, "max_actions") : profile.max_actions_per_slot};
            const size_t per_account{request.exists("per_account") ? Number<size_t>(request, "per_account") : maximum};
            Need(maximum > 0 && maximum <= profile.max_actions_per_slot && per_account > 0 && per_account <= maximum,
                 "candidate selection budget exceeds the configured profile");
            std::map<AccountId, size_t> counts;
            const admission::WithholdFilter fair{[&](const ActionV2& action) { return counts[action.Account()]++ >= per_account; }};
            taken = Must(pool->TakeForSlot(owner->Head()->Header().height + 1, maximum, fair));
            inputs.body.originals = taken->originals;
        }
        // Pool-selected actions the engine refuses are left out and become
        // terminal EXCLUDED (M4-ADM-03); explicitly listed actions are previewed as given.
        std::optional<state::Candidate> selected;
        size_t excluded{0};
        if (taken) {
            auto selection{Must(state::PreviewExcludingRefused(*owner, state::ParentRef::Committed(*owner->Head()), inputs))};
            for (const size_t position : selection.excluded) Must(pool->Exclude(taken->ids[position], taken->height));
            excluded = selection.excluded.size();
            selected.emplace(std::move(selection.candidate));
        } else {
            selected.emplace(Must(owner->Preview(state::ParentRef::Committed(*owner->Head()), inputs)));
        }
        const state::Candidate& candidate{*selected};
        UniValue out{UniValue::VOBJ}; out.pushKV("header", HexStr(candidate.Header().Encode())); out.pushKV("body", HexStr(candidate.Body().Encode()));
        const uint64_t view{pbft ? pbft->CurrentView() : 0};
        out.pushKV("hash", HexStr(candidate.Header().Hash())); out.pushKV("commit_digest", HexStr(VersionedCommitDigest(candidate.Header(), view)));
        if (pbft_view_change) out.pushKV("view", view);
        UniValue actions{UniValue::VARR};
        for (const auto& value : candidate.Body().originals) actions.push_back(HexStr(value));
        out.pushKV("actions", actions);
        out.pushKV("excluded", excluded);
        return out;
    }
    template <class Call> void RunAgreement(Call call, bool timeout = false)
    {
        Need(pbft_mode && pbft, "open explicitly with a PBFT agreement mode");
        const auto before_signer{signer_journal->Status()}, before_commit{commit_journal->Status()};
        const auto before_control{control_journal ? control_journal->Status() : storage::JournalStatus{}};
        const auto result{call()};
        const auto after_signer{signer_journal->Status()}, after_commit{commit_journal->Status()};
        const auto after_control{control_journal ? control_journal->Status() : storage::JournalStatus{}};
        const bool changed{after_signer.appended_seq != before_signer.appended_seq ||
                           after_commit.appended_seq != before_commit.appended_seq ||
                           after_control.appended_seq != before_control.appended_seq};
        if (changed) batch_locked = true;
        if (!result.has_value()) {
            const auto error{result.error()};
            const bool fatal{changed || after_signer.fatal || after_commit.fatal || after_control.fatal || error.code == ErrorCode::STORAGE_FATAL};
            // Fixed-view timeout is a signing fence, not a reason to discard
            // a later real certificate. A storage failure still terminates.
            if (timeout && !fatal && error.code == ErrorCode::FENCED) { pbft_timed_out = true; return; }
            Fail(error.code, error.detail, fatal);
        }
        if (pbft_view_change) {
            // OFFER and NEW_VIEW can select different values. Only a real
            // accepted-proposal record may extend this refusal index.
            const storage::ProposalKey key{authority.epoch, owner->Head()->Header().height + 1, pbft->CurrentView()};
            const auto checked{signer_log->AcceptedAt(key, profile)};
            if (!checked.has_value()) Fail(checked.error().code, checked.error().detail, true);
            const auto& accepted{*checked};
            if (accepted) {
                const auto [entry, inserted]{accepted_headers.emplace(key, accepted->header.Hash())};
                if (!inserted && entry->second != accepted->header.Hash()) Fail(ErrorCode::INVARIANT, "accepted-header index differs from durable signer history", true);
            }
        }
    }
    void CheckAccepted(const SlotHeaderV2& header, uint64_t view) const
    {
        const auto found{accepted_headers.find(storage::ProposalKey{header.authority_epoch, header.height, view})};
        if (found != accepted_headers.end() && found->second != header.Hash()) {
            Fail(ErrorCode::EQUIVOCATION_REFUSED, "proposal differs from the original durable accepted header");
        }
    }
    UniValue DrainAgreement()
    {
        Need(pbft_mode && pbft && sink, "open explicitly with a PBFT agreement mode");
        UniValue applied_decisions{UniValue::VARR};
        while (auto decision = pbft->TakeDecision()) {
            // TakeDecision is a one-shot, real B-DECISION witness. From here
            // every failure requires reopening the original journals; startup
            // then completes any verified decided-but-unapplied tail.
            critical = true;
            const auto exact_header{decision->header ? decision->header : pbft_header};
            Need(exact_header && exact_header->Hash() == decision->decision.Binding().header_hash &&
                 exact_header->height == decision->decision.Slot() &&
                 decision->certificate.Hash() == decision->decision.Binding().cert_hash,
                 "agreement decision differs from its exact durable header/certificate");
            const auto header{*exact_header};
            CheckCertificate(header, decision->certificate); CheckObserved(header, decision->body);
            auto candidate{Must(owner->Reexecute(header, decision->body))};
            const auto outcomes{candidate.Outcomes()};
            const auto installed{Must(owner->Apply(std::move(candidate), decision->decision, authority))};
            const auto pending{Must(commit_log->AppendApplied(installed.receipt))};
            Must(commit_log->Barrier());
            const auto durable{Must(commit_log->Confirm(pending))};
            Must(read->Publish(durable, Must(read::SlotView::Build(*installed.head, decision->certificate, outcomes))));
            Must(pool->OnCommitted(durable, *installed.head, outcomes, decision->body));
            certificates.emplace(header.height, decision->certificate);
            decisions.emplace(header.height, storage::DecisionRecord{header, decision->certificate, decision->body});
            UniValue item{UniValue::VOBJ}, encoded_outcomes{UniValue::VARR};
            item.pushKV("height", header.height); item.pushKV("head_hash", HexStr(header.Hash())); item.pushKV("state_root", HexStr(header.state_root));
            item.pushKV("header", HexStr(header.Encode())); item.pushKV("body", HexStr(decision->body.Encode()));
            item.pushKV("certificate", HexStr(decision->certificate.Encode()));
            for (const auto& outcome : outcomes) encoded_outcomes.push_back(HexStr(outcome.Encode()));
            item.pushKV("outcomes", std::move(encoded_outcomes)); applied_decisions.push_back(std::move(item));
            observed.reset(); recovered_observation.reset(); batch_locked = false; pbft_header.reset(); critical = false;
        }
        UniValue frames{UniValue::VARR};
        for (const auto& frame : sink->frames) {
            UniValue item{UniValue::VOBJ}; item.pushKV("from_seat", seat); item.pushKV("to_seat", frame.to);
            item.pushKV("message", HexStr(frame.bytes)); item.pushKV("durable_vote", frame.durable_vote);
            if (frame.durable_control) item.pushKV("durable_control", true);
            frames.push_back(std::move(item));
        }
        // Clear only after all response objects have been constructed. A pipe
        // failure is a process/reconnect gate, not a successful delivery ACK.
        sink->frames.clear();
        UniValue out{UniValue::VOBJ}; out.pushKV("frames", std::move(frames));
        out.pushKV("applied", std::move(applied_decisions)); out.pushKV("info", Info()); return out;
    }
    UniValue PbftStart(const UniValue& request)
    {
        Need(pbft_mode && pbft, "open explicitly with a PBFT agreement mode");
        const auto header{Must(SlotHeaderV2::Decode(Hex(request, "header", SLOT_HEADER_BYTES, SLOT_HEADER_BYTES)))};
        const auto body{Must(SlotBody::Decode(Hex(request, "body", SlotBody::MaxEncodedSize(profile)), profile))};
        CheckObserved(header, body);
        if (!pbft_view_change) CheckAccepted(header, 0);
        const auto candidate{Must(owner->Reexecute(header, body))};
        RunAgreement([&] { return pbft->Propose(candidate, authority); });
        if (!pbft_view_change) {
            accepted_headers.emplace(storage::ProposalKey{header.authority_epoch, header.height, 0}, header.Hash());
            pbft_header = header;
        }
        return DrainAgreement();
    }
    UniValue PbftReceive(const UniValue& request)
    {
        Need(pbft_mode && pbft, "open explicitly with a PBFT agreement mode");
        const uint32_t from{Number<uint32_t>(request, "from_seat")};
        Need(from < 4 && from != seat, "agreement peer outside configured local transport roster");
        const auto bytes{Hex(request, "message", agreement::MaxMessageBytes(profile))};
        const auto message{Must(agreement::DecodeMessage(bytes, profile))};
        std::optional<SlotHeaderV2> accepted_header;
        uint64_t accepted_view{0};
        if (const auto* proposal{std::get_if<agreement::Proposal>(&message)}) {
            CheckAccepted(proposal->header, proposal->view); accepted_header = proposal->header; accepted_view = proposal->view;
        }
        else if (const auto* final{std::get_if<agreement::CertifiedProposal>(&message)}) { accepted_header = final->proposal.header; accepted_view = final->proposal.view; }
        RunAgreement([&] { return pbft->OnMessage(from, bytes); });
        if (accepted_header && accepted_header->height > owner->Head()->Header().height) {
            pbft_header = *accepted_header;
            if (!pbft_view_change) accepted_headers.emplace(storage::ProposalKey{accepted_header->authority_epoch, accepted_header->height, accepted_view}, accepted_header->Hash());
        }
        return DrainAgreement();
    }
    UniValue Vote(const UniValue& request)
    {
        const auto header{Must(SlotHeaderV2::Decode(Hex(request, "header", SLOT_HEADER_BYTES, SLOT_HEADER_BYTES)))};
        const auto body{Must(SlotBody::Decode(Hex(request, "body", SlotBody::MaxEncodedSize(profile)), profile))};
        CheckObserved(header, body);
        const auto candidate{Must(owner->Reexecute(header, body))}; (void)candidate;
        Need(header.domain == domain && header.Authority() == authority, "vote differs from pinned authority");
        if (const auto decided{commit_log->DecisionAt(header.height)}; decided && decided->header_hash != header.Hash()) Fail(ErrorCode::EQUIVOCATION_REFUSED, "height already decided");
        critical = true;
        const auto proposal{Must(signer_log->AppendAcceptedProposal(storage::ProposalKey{0, header.height, 0}, header, body))};
        const storage::VoteKey key{domain, 0, header.height, 0, storage::VotePhase::COMMIT};
        const auto intent{Must(signer_log->AppendIntent(key, header.CommitDigest(), proposal))};
        Must(signer_log->Barrier());
        const auto durable_intent{Must(signer_log->Confirm(intent))};
        const auto signature{Must(signer->Sign(durable_intent))};
        const auto signed_vote{Must(signer_log->AppendSignedVote(durable_intent, signature))};
        Must(signer_log->Barrier());
        const auto durable_vote{Must(signer_log->Confirm(signed_vote))}; (void)durable_vote;
        critical = false; batch_locked = true;
        UniValue out{UniValue::VOBJ}; out.pushKV("seat", seat); out.pushKV("signature", HexStr(signature));
        out.pushKV("header_hash", HexStr(header.Hash())); out.pushKV("commit_digest", HexStr(header.CommitDigest())); return out;
    }
    UniValue CertificateFromVotes(const UniValue& request) const
    {
        const auto header{Must(SlotHeaderV2::Decode(Hex(request, "header", SLOT_HEADER_BYTES, SLOT_HEADER_BYTES)))};
        Need(header.domain == domain && header.Authority() == authority &&
             header.commitment_version == ExpectedCommitmentVersion(profile), "aggregation header differs from configured authority");
        Need(request["votes"].isArray() && request["votes"].size() >= 3 && request["votes"].size() <= 4, "three or four independent votes required");
        std::map<uint32_t, bls::Signature> votes;
        const uint256 digest{header.CommitDigest()};
        const std::span<const unsigned char> message{digest.begin(), digest.end()};
        for (const auto& item : request["votes"].getValues()) {
            const uint32_t index{Number<uint32_t>(item, "seat")};
            Need(index < 4, "vote index outside fixed roster");
            const auto signature{bls::Signature::Decode(Hex(item, "signature", bls::SIGNATURE_SIZE, bls::SIGNATURE_SIZE))};
            Need(signature && bls::Verify(seats->Keys()[index].Key(), message, *signature), "vote does not verify for its seat/header");
            Need(votes.emplace(index, *signature).second, "duplicate seat vote");
        }
        std::vector<bls::Signature> signatures;
        std::vector<uint32_t> indices;
        UniValue signers{UniValue::VARR};
        // One deterministic certificate for the supplied set; select the
        // three lowest seat indices. No signing or journal mutation occurs.
        for (const auto& [index, signature] : votes) {
            if (indices.size() == 3) break;
            indices.push_back(index); signatures.push_back(signature); signers.push_back(index);
        }
        const auto aggregate{bls::AggregateSignatures(signatures)};
        Need(aggregate.has_value(), "aggregate construction failed");
        SlotCertificate cert;
        cert.header_hash = header.Hash(); cert.bitmap = Must(SlotCertificate::MakeBitmap(4, indices));
        cert.aggregate = aggregate->Compressed(); CheckCertificate(header, cert);
        UniValue out{UniValue::VOBJ}; out.pushKV("certificate", HexStr(cert.Encode())); out.pushKV("signers", std::move(signers));
        out.pushKV("header_hash", HexStr(header.Hash())); return out;
    }
    UniValue Commit(const UniValue& request)
    {
        const auto header{Must(SlotHeaderV2::Decode(Hex(request, "header", SLOT_HEADER_BYTES, SLOT_HEADER_BYTES)))};
        const auto body{Must(SlotBody::Decode(Hex(request, "body", SlotBody::MaxEncodedSize(profile)), profile))};
        const auto cert{Must(SlotCertificate::Decode(Hex(request, "certificate", SlotCertificate::MAX_ENCODED_BYTES)))};
        CheckCertificate(header, cert);
        Need(storage::BodyMatchesHeader(header, body), "committed body differs from certificate header");
        if (header.height <= owner->Head()->Header().height) {
            const auto found{decisions.find(header.height)};
            Need(found != decisions.end() && found->second.header == header && found->second.body == body, "committed height differs from retained decision");
            UniValue out{Info()}; out.pushKV("duplicate", true); return out;
        }
        CheckObserved(header, body);
        auto candidate{Must(owner->Reexecute(header, body))};
        auto outcomes{candidate.Outcomes()};
        critical = true;
        const auto pending{Must(commit_log->AppendDecision(header, cert, body))};
        Must(commit_log->Barrier());
        const auto decision{Must(commit_log->Confirm(pending))};
        const auto installed{Must(owner->Apply(std::move(candidate), decision, authority))};
        const auto applied{Must(commit_log->AppendApplied(installed.receipt))};
        Must(commit_log->Barrier());
        const auto durable{Must(commit_log->Confirm(applied))};
        Must(read->Publish(durable, Must(read::SlotView::Build(*installed.head, cert, outcomes))));
        Must(pool->OnCommitted(durable, *installed.head, outcomes, body));
        certificates.emplace(header.height, cert); decisions.emplace(header.height, storage::DecisionRecord{header, cert, body});
        observed.reset(); recovered_observation.reset(); batch_locked = false; critical = false;
        UniValue out{Info()}, encoded_outcomes{UniValue::VARR};
        for (const auto& outcome : outcomes) encoded_outcomes.push_back(HexStr(outcome.Encode()));
        out.pushKV("outcomes", std::move(encoded_outcomes)); out.pushKV("duplicate", false); return out;
    }
    void Envelope(UniValue& out) const
    {
        const auto head{owner->Head()}; out.pushKV("header", HexStr(head->Header().Encode()));
        out.pushKV("roots", RootsJson(Must(head->State().engine.Commit())));
        if (head->Header().height) {
            const auto& cert{certificates.at(head->Header().height)}; CheckCertificate(head->Header(), cert);
            out.pushKV("certificate", HexStr(cert.Encode()));
        } else out.pushKV("certificate", UniValue{});
    }
    UniValue Account(const UniValue& request) const
    {
        const AccountId account{JsonHash(request, "account")}; const auto head{owner->Head()};
        const auto& ledger{head->State().engine.ledger};
        UniValue out{UniValue::VOBJ}; out.pushKV("account", HexStr(account.id)); out.pushKV("found", ledger.HasAccount(account));
        Envelope(out);
        if (!ledger.HasAccount(account)) return out;
        const auto witness{Must(ledger::ProveAccountV0(ledger, account))}; const auto roots{Must(head->State().engine.Commit())};
        Must(verify::VerifyAccountLeaf(head->Header(), witness.leaf, roots, witness.proof, profile));
        UniValue balances{UniValue::VARR};
        for (const auto& balance : witness.leaf.balances) {
            UniValue item{UniValue::VOBJ}; item.pushKV("asset", HexStr(balance.asset.id)); item.pushKV("available", balance.available); item.pushKV("reserved", balance.reserved); balances.push_back(std::move(item));
        }
        out.pushKV("balances", std::move(balances)); out.pushKV("leaf", HexStr(witness.leaf.Encode()));
        out.pushKV("proof", ProofJson(witness.proof)); out.pushKV("verified", head->Header().height > 0);
        const auto& nonces{witness.leaf.used_nonces};
        if (nonces.empty()) out.pushKV("next_nonce", uint64_t{0});
        else if (nonces.back() != UINT64_MAX) out.pushKV("next_nonce", nonces.back() + 1);
        return out;
    }
    UniValue Withdrawal(const UniValue& request) const
    {
        const ActionId id{JsonHash(request, "withdrawal_id")}; const auto head{owner->Head()}; const auto& ledger{head->State().engine.ledger};
        UniValue out{UniValue::VOBJ}; out.pushKV("found", ledger.Withdrawals().contains(id)); Envelope(out);
        if (!ledger.Withdrawals().contains(id)) return out;
        const auto witness{Must(ledger::ProveWithdrawal(ledger, id))}; const auto roots{Must(head->State().engine.Commit())};
        Must(verify::VerifyWithdrawal(head->Header(), witness.record, roots, witness.proof, profile));
        const auto& record{witness.record}; UniValue item{UniValue::VOBJ}, event{UniValue::VOBJ};
        item.pushKV("id", HexStr(record.id.id)); item.pushKV("account", HexStr(record.account.id)); item.pushKV("asset", HexStr(record.asset.id));
        item.pushKV("amount", record.amount); item.pushKV("fee", record.fee); item.pushKV("recipient", HexStr(record.recipient));
        item.pushKV("slot", record.slot); item.pushKV("paid", record.paid); event.pushKV("txid", HexStr(record.payout_event.txid)); event.pushKV("vout", record.payout_event.vout);
        item.pushKV("payout_event", std::move(event)); out.pushKV("record", std::move(item)); out.pushKV("record_hex", HexStr(record.Encode()));
        out.pushKV("proof", ProofJson(witness.proof)); out.pushKV("verified", true); return out;
    }
    UniValue WithdrawalPending(const UniValue& request) const
    {
        const uint32_t limit{request.exists("limit") ? Number<uint32_t>(request, "limit") : 32};
        Need(limit > 0 && limit <= 64, "withdrawal pending page must contain one to 64 claims");
        const auto& withdrawals{owner->Head()->State().engine.ledger.Withdrawals()};
        auto item{request.exists("cursor") && !request["cursor"].isNull() ? withdrawals.upper_bound(ActionId{JsonHash(request, "cursor")}) : withdrawals.begin()};
        UniValue ids{UniValue::VARR}; std::optional<ActionId> last;
        size_t scanned{0};
        for (; item != withdrawals.end() && scanned < 256 && ids.size() < limit; ++item, ++scanned) {
            last = item->first;
            if (!item->second.paid) ids.push_back(HexStr(item->first.id));
        }
        UniValue out{UniValue::VOBJ}; out.pushKV("withdrawal_ids", ids); out.pushKV("more", item != withdrawals.end());
        if (item != withdrawals.end() && last) out.pushKV("next_cursor", HexStr(last->id)); else out.pushKV("next_cursor", UniValue{});
        return out;
    }
    UniValue WithdrawalAuthority(const UniValue& request)
    {
        const ActionId id{JsonHash(request, "withdrawal_id")};
        const auto& current{owner->Head()->State().engine.ledger.Withdrawals()};
        const auto found{current.find(id)};
        if (found == current.end()) { UniValue out{UniValue::VOBJ}; out.pushKV("found", false); return out; }
        Need(!found->second.paid, "paid withdrawal cannot receive a fresh custody authorization");
        if (const auto cached{withdrawal_authorities.find(id)}; cached != withdrawal_authorities.end()) {
            Need(HexStr(found->second.Encode()) == Text(cached->second, "record_hex"), "historical withdrawal authority differs from the current unpaid obligation");
            return cached->second;
        }
        const auto before{commit_journal->Status()};
        auto historical{Must(state::StateOwner::Create(profile, *genesis, commit_instance))};
        const uint64_t original_slot{found->second.slot};
        for (uint64_t height{1}; height <= original_slot; ++height) {
            const auto old{decisions.find(height)};
            Need(old != decisions.end(), "withdrawal authority lacks its original contiguous certified history");
            const auto& record{old->second};
            const auto known{commit_log->DecisionAt(height)};
            Need(known && known->header_hash == record.header.Hash() && known->cert_hash == record.certificate.Hash(),
                 "historical withdrawal replay cannot introduce a fresh decision");
            auto candidate{Must(historical->Reexecute(record.header, record.body))};
            const auto pending{Must(commit_log->AppendDecision(record.header, record.certificate, record.body))};
            const auto token{Must(commit_log->Confirm(pending))};
            const auto applied{Must(historical->Apply(std::move(candidate), token, authority))}; (void)applied;
        }
        if (commit_journal->Status() != before) Fail(ErrorCode::STORAGE_FATAL, "read-only historical authority changed the durable journal", true);
        const auto head{historical->Head()};
        const auto witness{Must(ledger::ProveWithdrawal(head->State().engine.ledger, id))};
        Need(witness.record == found->second && !witness.record.paid, "original withdrawal proof differs from the exact current unpaid claim");
        const auto roots{Must(head->State().engine.Commit())};
        Must(verify::VerifyWithdrawal(head->Header(), witness.record, roots, witness.proof, profile));
        const auto& decision{decisions.at(original_slot)}; CheckCertificate(head->Header(), decision.certificate);
        UniValue out{UniValue::VOBJ}, value{UniValue::VOBJ}, event{UniValue::VOBJ};
        const auto& record{witness.record};
        value.pushKV("id", HexStr(record.id.id)); value.pushKV("account", HexStr(record.account.id)); value.pushKV("asset", HexStr(record.asset.id));
        value.pushKV("amount", record.amount); value.pushKV("fee", record.fee); value.pushKV("recipient", HexStr(record.recipient));
        value.pushKV("slot", record.slot); value.pushKV("paid", false); event.pushKV("txid", HexStr(record.payout_event.txid)); event.pushKV("vout", record.payout_event.vout);
        value.pushKV("payout_event", event);
        out.pushKV("found", true); out.pushKV("verified", true); out.pushKV("current_unpaid", true);
        out.pushKV("record", value); out.pushKV("current_record", value); out.pushKV("record_hex", HexStr(record.Encode()));
        out.pushKV("header", HexStr(head->Header().Encode())); out.pushKV("head_hash", HexStr(head->Header().Hash()));
        out.pushKV("roots", RootsJson(roots)); out.pushKV("proof", ProofJson(witness.proof)); out.pushKV("certificate", HexStr(decision.certificate.Encode()));
        if (profile.source_range_commitment) {
            Need(!decision.body.facts.empty(), "historical authority lacks its committed source checkpoint");
            const auto fact{Must(DecodeFact(decision.body.facts.back()))};
            const auto* range{std::get_if<SourceRangeFact>(&fact)};
            Need(range && range->domain == domain && range->chain_id == profile.source_chain_id && range->confirmations == profile.source_confirmations,
                 "historical authority has another source checkpoint policy");
            UniValue point{UniValue::VOBJ}; point.pushKV("height", range->to_height); point.pushKV("hash", "0x" + HexStr(range->to_hash));
            point.pushKV("minimum_confirmations", range->confirmations); out.pushKV("source_checkpoint", point);
        }
        if (withdrawal_authorities.size() >= 4096) {
            for (auto it{withdrawal_authorities.begin()}; it != withdrawal_authorities.end();) {
                const auto now{current.find(it->first)};
                if (now == current.end() || now->second.paid) it = withdrawal_authorities.erase(it); else ++it;
            }
        }
        Need(withdrawal_authorities.size() < 4096, "historical withdrawal authority cache is full");
        withdrawal_authorities.emplace(id, out); return out;
    }
    UniValue Totals() const
    {
        const auto& ledger{owner->Head()->State().engine.ledger}; Must(ledger.CheckConservation());
        UniValue out{UniValue::VOBJ}, assets{UniValue::VARR};
        for (const auto& [asset, totals] : ledger.Totals()) {
            Atoms available{0}, reserved{0}, protocol{0};
            for (const auto& [account, state] : ledger.Accounts()) {
                const auto balance{state.BalanceOf(asset)}; available = Must(AddAtoms(available, balance.available));
                reserved = Must(AddAtoms(reserved, balance.reserved));
                if (ledger.IsProtocolAccount(account)) protocol = Must(AddAtoms(protocol, balance.available));
            }
            UniValue item{UniValue::VOBJ}; item.pushKV("asset", HexStr(asset.id)); item.pushKV("deposits", totals.deposits);
            item.pushKV("effects", totals.effects); item.pushKV("pending", totals.pending); item.pushKV("fees_held", totals.fees_held);
            item.pushKV("available", available); item.pushKV("reserved", reserved); item.pushKV("protocol_available", protocol);
            const Atoms accounts{Must(AddAtoms(available, reserved))}; item.pushKV("account_liabilities", accounts);
            item.pushKV("total_liabilities", Must(AddAtoms(Must(AddAtoms(accounts, totals.pending)), totals.fees_held))); assets.push_back(std::move(item));
        }
        out.pushKV("assets", std::move(assets)); out.pushKV("conservation", true); Envelope(out); return out;
    }
    UniValue Handle(const UniValue& request)
    {
        const auto cmd{Text(request, "cmd")};
        if (cmd == "info") return Info();
        if (observation_authority) {
            const bool passive{cmd == "account" || cmd == "balances" || cmd == "withdrawal" || cmd == "withdrawal_authority" || cmd == "withdrawal_pending" || cmd == "totals" ||
                cmd == "observe" || cmd == "check_observation" || cmd == "candidate" || cmd == "propose" ||
                cmd == "network_poll" || cmd == "network_send" || cmd == "network_drop" || cmd == "network_history" ||
                cmd == "network_check_history" || cmd == "network_observation_authority" || cmd == "network_own_observation_authority" || cmd == "network_history_observation_authority" ||
                cmd == "network_resume_observation" || cmd == "network_fault_drop_commits"};
            const bool selected_dispatch{cmd == "network_dispatch" && Number<uint64_t>(request, "token") == observation_authority->token};
            Need(passive || selected_dispatch, "ordinary agreement is paused until the verified source handover finishes");
        }
        if (cmd == "observe") return Observe(request);
        if (cmd == "check_observation") return CheckObservation(request);
        if (cmd == "candidate" || cmd == "propose") return Candidate(request);
        if (cmd == "pbft_start") return PbftStart(request);
        if (cmd == "pbft_receive") { Need(!network_mode, "caller-supplied peer identity is disabled in authenticated network mode"); return PbftReceive(request); }
        if (cmd == "network_endpoint") return NetworkEndpoint();
        if (cmd == "network_start") return NetworkStart(request);
        if (cmd == "network_poll") return NetworkPoll();
        if (cmd == "network_send") return NetworkSend(request);
        if (cmd == "network_dispatch") return NetworkDispatch(request);
        if (cmd == "network_check_history") return NetworkCheckHistory(request);
        if (cmd == "network_observation_authority") return NetworkObservationAuthority(request);
        if (cmd == "network_own_observation_authority") return NetworkOwnObservationAuthority(request);
        if (cmd == "network_history_observation_authority") return NetworkHistoryObservationAuthority(request);
        if (cmd == "network_resume_observation") return NetworkResumeObservation();
        if (cmd == "network_fault_drop_commits") {
            Need(network_mode && sink && request["enabled"].isBool(), "commit-loss fault is a private REGTEST-only switch");
            sink->regtest_drop_commits = request["enabled"].get_bool(); return Info();
        }
        if (cmd == "network_history") return NetworkHistory(request);
        if (cmd == "network_drop") {
            Need(!observation_authority || Number<uint64_t>(request, "token") != observation_authority->token,
                 "cannot drop the in-progress source handover capability");
            Need(network_pending.erase(Number<uint64_t>(request, "token")) == 1, "network receive token is unknown or consumed");
            UniValue out{UniValue::VOBJ}; out.pushKV("consumed", true); return out;
        }
        if (cmd == "pbft_drain") return DrainAgreement();
        if (cmd == "pbft_timeout") {
            RunAgreement([&] { return pbft->OnTimeout(Number<uint64_t>(request, "now_ms")); }, true);
            return DrainAgreement();
        }
        if (cmd == "vote") { Need(!pbft_mode, "direct vote disabled in PBFT mode"); return Vote(request); }
        if (cmd == "cert_from_votes") { Need(!pbft_view_change, "direct certificate aggregation disabled in view-change mode"); return CertificateFromVotes(request); }
        if (cmd == "commit" || cmd == "apply") { Need(!pbft_mode, "direct commit disabled in PBFT mode"); return Commit(request); }
        if (cmd == "account" || cmd == "balances") return Account(request);
        if (cmd == "withdrawal") return Withdrawal(request);
        if (cmd == "withdrawal_authority") return WithdrawalAuthority(request);
        if (cmd == "withdrawal_pending") return WithdrawalPending(request);
        if (cmd == "totals") return Totals();
        if (cmd == "submit") {
            const auto frame{Hex(request, "frame", ActionV2::MaxEncodedSize(profile))};
            const auto reply{Must(admission->Admit(frame))};
            UniValue out{UniValue::VOBJ}; out.pushKV("code", std::string{ToString(reply.code)}); out.pushKV("reason", std::string{reply.reason});
            out.pushKV("pending", reply.pending.has_value()); out.pushKV("included", reply.included.has_value());
            if (reply.outcome) out.pushKV("outcome", HexStr(reply.outcome->outcome.outcome.Encode()));
            if (reply.pending || reply.included) out.pushKV("account", HexStr(Must(ActionV2::Decode(frame, profile)).Account().id));
            if (reply.id) out.pushKV("id", HexStr(reply.id->id));
            return out;
        }
        Fail(ErrorCode::INVALID_INPUT, "unknown operator command");
    }
};

RegtestOperator::RegtestOperator(std::unique_ptr<Impl> impl) : m_impl{std::move(impl)} {}
RegtestOperator::~RegtestOperator() = default;
std::unique_ptr<RegtestOperator> RegtestOperator::Open(const UniValue& config)
{
    return std::unique_ptr<RegtestOperator>{new RegtestOperator{std::make_unique<Impl>(config)}};
}
UniValue RegtestOperator::Handle(const UniValue& request)
{
    try { return m_impl->Handle(request); }
    catch (const OperatorFailure& failure) {
        if (m_impl->critical) throw OperatorFailure{failure.code, failure.what(), true};
        throw;
    }
}

} // namespace flowmesh2::dev
