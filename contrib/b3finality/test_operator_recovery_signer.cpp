// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// Standalone tests of the PRODUCTION signer, pool and durable store. The
// indexed headers and tracker state are explicitly synthetic: this does not
// test tracker derivation, block validation, real-chain agreement or quorum.
// No BasicTestingSetup, mining, wallet, network or production data directory.

#include <chain.h>
#include <modern/chain_domain.h>
#include <node/finality_signature.h>
#include <node/finality_signing_policy.h>
#include <node/validator_set.h>
#include <random.h>
#include <util/translation.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

const TranslateFn G_TRANSLATION_FUN{nullptr};

namespace {
size_t checks{0};
size_t cases{0};

void Check(const bool condition, const std::string& description)
{
    ++checks;
    if (!condition) throw std::runtime_error(description);
}

uint256 NonzeroHash(FastRandomContext& rng)
{
    uint256 value;
    do { value = rng.rand256(); } while (value.IsNull());
    return value;
}

class TemporaryDirectory
{
    fs::path m_path;

public:
    explicit TemporaryDirectory(FastRandomContext& rng)
    {
        const fs::path parent{std::filesystem::temp_directory_path()};
        for (int attempt{0}; attempt < 16; ++attempt) {
            const fs::path candidate{parent / fs::u8path(
                "b3-operator-signer-test-" + NonzeroHash(rng).GetHex())};
            if (std::filesystem::create_directory(candidate.std_path())) {
                m_path = candidate;
                return;
            }
        }
        throw std::runtime_error("could not create a fresh test directory");
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    ~TemporaryDirectory()
    {
        if (m_path.empty()) return;
        std::error_code error;
        // Only the exact unpredictable directory created by this object.
        std::filesystem::remove_all(m_path.std_path(), error);
        if (error) std::cerr << "Could not remove synthetic test directory: "
                             << fs::PathToString(m_path) << '\n';
    }
    const fs::path& Path() const { return m_path; }
};

bls::SecretKey NewKey(FastRandomContext& rng)
{
    const auto ikm{NonzeroHash(rng)};
    const auto key{bls::SecretKey::FromIKM(std::span<const unsigned char>{ikm.begin(), 32})};
    Check(key.has_value(), "synthetic BLS key generation");
    return *key;
}

struct Fixture
{
    FastRandomContext rng;
    TemporaryDirectory directory{rng};
    Consensus::Params params;
    node::FinalityTracker tracker;
    std::array<uint256, 100> hashes;
    std::array<CBlockIndex, 100> blocks;
    CChain chain;
    bls::SecretKey key{NewKey(rng)};
    bls::SecretKey other_key{NewKey(rng)};
    modern::ValidatorKeyBytes validator{};
    modern::ValidatorKeyBytes other_validator{};
    uint256 domain;
    std::shared_ptr<const node::ValidatorSetSnapshot> set0;
    node::FinalitySignaturePool pool;
    node::FinalitySignerState original;
    Consensus::FinalitySignerRecovery pin;

    Fixture()
    {
        params.legacy_b3coin = true;
        params.hard_fork_height = 1;
        params.transition_pow_length = 0;
        params.legacy_final_hash = NonzeroHash(rng);
        params.hashGenesisBlock = NonzeroHash(rng);
        params.modern_pos.emplace();
        params.modern_pos->checkpoint_interval = 10;
        params.modern_pos->checkpoint_depth = 12;
        params.modern_pos->min_finality_set = 2;
        const auto configured{modern::ModernChainDomain(
            params.hashGenesisBlock, *params.legacy_final_hash)};
        Check(configured.has_value(), "synthetic domain");
        domain = *configured;
        for (size_t i{0}; i < blocks.size(); ++i) {
            hashes[i] = NonzeroHash(rng);
            blocks[i].nHeight = static_cast<int>(i);
            blocks[i].phashBlock = &hashes[i];
            blocks[i].pprev = i == 0 ? nullptr : &blocks[i - 1];
        }
        chain.SetTip(blocks[0]);
        validator[0] = 1;
        other_validator[0] = 2;
        Check(bls::VerifyPoP(key.GetPublicKey(), key.SignPoP()) &&
              bls::VerifyPoP(other_key.GetPublicKey(), other_key.SignPoP()),
              "synthetic binding keys have valid proofs of possession");
        node::FinalityBindingIndex bindings;
        bindings.ConnectBlock(0, {
            {validator, {key.GetPublicKey().Compressed(), 0, 0}},
            {other_validator, {other_key.GetPublicKey().Compressed(), 0, 0}},
        });
        const auto snapshot{node::ValidatorSetSnapshot::Build(0, {
            {validator, modern::FINALITY_WEIGHT_UNIT},
            {other_validator, modern::FINALITY_WEIGHT_UNIT},
        }, bindings)};
        Check(snapshot.has_value(), "synthetic two-validator snapshot");
        set0 = std::make_shared<const node::ValidatorSetSnapshot>(*snapshot);
        State().bootstrapped = true;
        State().epoch_starts = {1};
        State().current = set0;
        State().next = std::make_shared<const node::ValidatorSetSnapshot>(set0->WithEpoch(1));

        // Obtain the incident by actual production signing, not by writing a
        // fabricated signed state. The setup signer retains its default12
        // timing so the recovery's independent20 floor can be isolated.
        {
            node::FinalitySigner signer;
            Arm(signer);
            Check(Sign(signer, 0).empty(), "safe pre-checkpoint journal initialization");
            const auto votes{Sign(signer, 13)};
            Check(votes.size() == 1 && votes.front().height == 1, "produce original checkpoint1 vote");
            Verify(votes.front());
        }
        original = Journal();
        pin.chain_domain = domain;
        pin.validator_key = validator;
        pin.incident_height = original.last_signed_height;
        pin.incident_block_hash = original.last_signed_block_hash;
        pin.incident_epoch = original.lock_epoch;
        pin.incident_signing_set_hash = original.lock_signing_set_hash;
        pin.incident_successor_set_hash = original.lock_successor_set_hash;
        pin.anchor_height = 11;
        pin.anchor_block_hash = hashes[11];
        Check(pin.Valid(), "synthetic exact incident and selected anchor");
        hashes[1] = NonzeroHash(rng); // this active branch no longer contains the vote
        pool = node::FinalitySignaturePool{};
    }

    node::FinalityTracker::State& State()
    {
        // Legal because tracker is genuinely non-const. This intentionally
        // supplies a SYNTHETIC derived-state observation, not a production
        // hook, and is not proof that these headers passed block consensus.
        return const_cast<node::FinalityTracker::State&>(tracker.Current());
    }

    void Arm(node::FinalitySigner& signer)
    {
        std::string error;
        const bool ok{signer.SetKeyPersistent(key, validator, domain, directory.Path(), error)};
        Check(ok, "load synthetic signer journal: " + error);
    }

    void Approve(node::FinalitySigner& signer, const Consensus::FinalitySignerRecovery& plan)
    {
        std::string error;
        const bool ok{signer.SetOperatorTrustedRecovery(plan, error)};
        Check(ok, "explicit synthetic operator approval: " + error);
        Check(Journal() == original, "approval alone must not write or rebase journal");
    }

    std::vector<node::FinalitySig> Sign(node::FinalitySigner& signer, const int tip)
    {
        chain.SetTip(blocks.at(tip));
        return signer.MaybeSign(tracker, chain, params, pool);
    }

    node::FinalitySignerState Journal()
    {
        node::FinalitySignerStore store;
        std::string error;
        const bool opened{store.Open(directory.Path(), domain, validator, error)};
        Check(opened && store.State().has_value(), "reload synthetic journal: " + error);
        return *store.State();
    }

    void Verify(const node::FinalitySig& sig)
    {
        const auto object{node::FinalitySignaturePool::ExpectedFinalizedBlock(
            sig.epoch, sig.height, tracker.Current(), chain, params)};
        Check(object.has_value(), "reconstruct emitted finality object");
        const auto digest{modern::FinalityDigest(domain, *object)};
        const auto signature{bls::Signature::Decode(sig.signature)};
        Check(signature.has_value() && bls::Verify(key.GetPublicKey(),
            std::span<const unsigned char>{digest.begin(), 32}, *signature),
            "emitted signature verifies against this active chain's exact digest");
        node::FinalitySignaturePool receiver;
        Check(receiver.Submit(sig, tracker, chain, params) ==
              node::FinalitySignaturePool::Accept::ACCEPTED,
              "independent production pool accepts emitted signature");
    }

    void CheckAnchorOnly()
    {
        const auto object{node::FinalitySignaturePool::ExpectedFinalizedBlock(
            pin.incident_epoch, pin.anchor_height, tracker.Current(), chain, params)};
        Check(object.has_value(), "selected anchor object is available");
        auto expected{original};
        expected.lock_height = pin.anchor_height;
        expected.lock_block_hash = pin.anchor_block_hash;
        expected.lock_digest = modern::FinalityDigest(domain, *object);
        Check(Journal() == expected, "only lock height/hash/digest change; original vote and identity/sets survive");
    }
};

void NoApprovalAndHardenedCompatibility()
{
    Fixture f;
    node::FinalitySigner signer;
    f.Arm(signer);
    Check(f.Sign(signer, 31).empty(), "no operator approval must retain orphan lock");
    Check(f.Journal() == f.original, "unapproved attempt leaves journal unchanged");
    f.params.finality_signer_recoveries = {f.pin};
    Check(f.Sign(signer, 23).empty(), "compiled pin still requires a hardened checkpoint");
    Check(f.Journal() == f.original, "missing hardened checkpoint cannot rebase");
    f.params.modern_checkpoints[11] = NonzeroHash(f.rng);
    Check(f.Sign(signer, 23).empty(), "compiled pin rejects different hardened hash");
    Check(f.Journal() == f.original, "wrong hardened checkpoint cannot rebase");
    f.params.modern_checkpoints[11] = f.pin.anchor_block_hash;
    Check(f.Sign(signer, 23).empty(), "existing hardened mode retains consensus12 depth");
    f.CheckAnchorOnly();
}

void OperatorDepthPreservationAndNextVote()
{
    Fixture f;
    node::FinalitySigner signer;
    f.Arm(signer);
    f.Approve(signer, f.pin);
    Check(f.params.modern_checkpoints.empty() && f.params.finality_signer_recoveries.empty(),
          "explicit mode does not install a consensus checkpoint or recovery pin");
    Check(f.Sign(signer, 30).empty(), "selected anchor19 deep must wait");
    Check(f.Journal() == f.original, "anchor19 wait preserves exact original journal");
    Check(f.Sign(signer, 31).empty(), "anchor20 rebases lock without a vote at or below anchor");
    f.CheckAnchorOnly();
    Check(signer.LastSignedHeight() == 1, "anchor movement is not reported as a signed vote");
    Check(f.Sign(signer, 32).empty(), "never backfill checkpoint11 below the ancestry lock");
    f.CheckAnchorOnly();
    node::FinalitySigner restarted;
    f.Arm(restarted);
    Check(f.Sign(restarted, 32).empty(), "restarted signer retains forward ancestry lock");
    const auto votes{f.Sign(restarted, 33)};
    Check(votes.size() == 1 && votes.front().height == 21, "next actual vote is strictly above selected anchor");
    f.Verify(votes.front());
    const auto after{f.Journal()};
    Check(after.last_signed_height == 21 && after.lock_height == 21,
          "next verified vote advances both durable watermarks");
    Check(f.Sign(restarted, 33).empty(), "restart does not permit repeated signing");
}

void StrongerConsensusDepth()
{
    Fixture f;
    f.params.modern_pos->checkpoint_depth = 25;
    node::FinalitySigner signer;
    f.Arm(signer);
    f.Approve(signer, f.pin);
    Check(f.Sign(signer, 35).empty(), "consensus25 overrides operator20 floor");
    Check(f.Journal() == f.original, "consensus24-deep wait preserves journal");
    Check(f.Sign(signer, 36).empty(), "consensus25-deep anchor becomes usable");
    f.CheckAnchorOnly();
}

void MainnetSigningPolicyDoesNotRaiseReceiverDepth()
{
    Fixture f;
    node::FinalitySigner signer{node::FinalitySigningPolicy::ForNetwork(ChainType::MAIN)};
    f.Arm(signer);
    f.Approve(signer, f.pin);
    Check(f.Sign(signer, 30).empty(), "mainnet operator recovery still waits for anchor20");
    Check(f.Journal() == f.original, "mainnet anchor19 wait cannot change journal");
    Check(f.Sign(signer, 31).empty(), "mainnet selected anchor20 can move lock without new vote");
    f.CheckAnchorOnly();
    Check(f.Sign(signer, 32).empty(), "mainnet candidate21 is only11 deep");

    // A DIFFERENT synthetic validator's legitimate vote is already acceptable
    // at consensus12, although our production-policy signer is still waiting.
    const auto object{node::FinalitySignaturePool::ExpectedFinalizedBlock(
        0, 21, f.tracker.Current(), f.chain, f.params)};
    Check(object.has_value(), "reconstruct peer's candidate21 object");
    const auto digest{modern::FinalityDigest(f.domain, *object)};
    node::FinalitySig peer_vote;
    peer_vote.height = 21;
    peer_vote.index = 1;
    peer_vote.signature = f.other_key.Sign(
        std::span<const unsigned char>{digest.begin(), 32}).Compressed();
    node::FinalitySignaturePool receiver;
    Check(receiver.Submit(peer_vote, f.tracker, f.chain, f.params) ==
          node::FinalitySignaturePool::Accept::TOO_SHALLOW,
          "received candidate21 vote at depth11 remains too shallow");
    Check(f.Sign(signer, 33).empty(), "mainnet local signer does not emit at candidate depth12");
    Check(receiver.Submit(peer_vote, f.tracker, f.chain, f.params) ==
          node::FinalitySignaturePool::Accept::ACCEPTED,
          "received valid candidate21 vote is accepted at unchanged consensus depth12");
    Check(f.Sign(signer, 40).empty(), "mainnet new vote waits through candidate depth19");
    f.CheckAnchorOnly();
    const auto votes{f.Sign(signer, 41)};
    Check(votes.size() == 1 && votes.front().height == 21,
          "mainnet policy emits exactly the next checkpoint at depth20");
    f.Verify(votes.front());
    Check(f.Journal().last_signed_height == 21,
          "mainnet next vote advances the durable watermark only after20");
}

void SetterRejectsDifferentJournalFacts()
{
    const std::vector<std::pair<std::string, std::function<void(Fixture&, Consensus::FinalitySignerRecovery&)>>> mutations{
        {"missing target", [](auto&, auto& p) { p.validator_key.reset(); }},
        {"zero target", [](auto&, auto& p) { p.validator_key->fill(0); }},
        {"wrong identity", [](auto& f, auto& p) { p.validator_key = f.other_validator; }},
        {"wrong domain", [](auto& f, auto& p) { p.chain_domain = NonzeroHash(f.rng); }},
        {"wrong incident height", [](auto&, auto& p) { ++p.incident_height; }},
        {"wrong incident hash", [](auto& f, auto& p) { p.incident_block_hash = NonzeroHash(f.rng); }},
        {"wrong epoch", [](auto&, auto& p) { ++p.incident_epoch; }},
        {"wrong signing set", [](auto& f, auto& p) { p.incident_signing_set_hash = NonzeroHash(f.rng); }},
        {"wrong successor set", [](auto& f, auto& p) { p.incident_successor_set_hash = NonzeroHash(f.rng); }},
        {"backward anchor", [](auto&, auto& p) { p.anchor_height = p.incident_height; }},
        {"null anchor", [](auto&, auto& p) { p.anchor_block_hash.SetNull(); }},
    };
    for (const auto& [label, mutate] : mutations) {
        Fixture f;
        node::FinalitySigner signer;
        f.Arm(signer);
        auto bad{f.pin};
        mutate(f, bad);
        std::string error;
        Check(!signer.SetOperatorTrustedRecovery(bad, error), label + " must be refused by setter");
        Check(!error.empty(), label + " rejection must be explained");
        Check(f.Sign(signer, 31).empty(), label + " must not implicitly approve recovery");
        Check(f.Journal() == f.original, label + " must preserve journal");
    }
}

void SetterRequiresLoadedPersistentJournal()
{
    Fixture f;
    std::string error;
    node::FinalitySigner unarmed;
    Check(!unarmed.SetOperatorTrustedRecovery(f.pin, error), "unarmed signer cannot approve recovery");
    unarmed.SetKey(f.key, f.validator);
    Check(!unarmed.SetOperatorTrustedRecovery(f.pin, error), "offline signer without durable store cannot approve");
    node::FinalitySigner absent;
    Check(absent.SetKeyPersistent(f.key, f.validator, f.domain,
          f.directory.Path() / "absent", error), "open separate synthetic absent journal");
    Check(!absent.SetOperatorTrustedRecovery(f.pin, error), "absent journal cannot approve recovery");
    Check(f.Sign(absent, 31).empty(), "operator mode cannot initialize an ambiguous missing journal");
    Check(f.Journal() == f.original, "missing-journal test cannot affect original journal");
}

void RuntimeChainAndLineageRefusals()
{
    const std::vector<std::pair<std::string, std::function<void(Fixture&)>>> mutations{
        {"wrong active anchor", [](auto& f) { f.hashes[11] = NonzeroHash(f.rng); }},
        {"lineage broken", [](auto& f) { f.State().lineage_broken = true; }},
        {"not bootstrapped", [](auto& f) { f.State().bootstrapped = false; }},
        {"missing current set", [](auto& f) { f.State().current.reset(); }},
        {"wrong live signing set", [](auto& f) {
            f.State().current = std::make_shared<const node::ValidatorSetSnapshot>(f.set0->WithEpoch(9));
        }},
        {"missing successor", [](auto& f) { f.State().next.reset(); }},
        {"wrong live successor set", [](auto& f) {
            f.State().next = std::make_shared<const node::ValidatorSetSnapshot>(f.set0->WithEpoch(9));
        }},
        {"expired incident epoch", [](auto& f) {
            f.State().epoch = 2;
            f.State().epoch_starts = {1, 21, 31};
            f.State().previous = std::make_shared<const node::ValidatorSetSnapshot>(f.set0->WithEpoch(1));
            f.State().current = std::make_shared<const node::ValidatorSetSnapshot>(f.set0->WithEpoch(2));
            f.State().next = std::make_shared<const node::ValidatorSetSnapshot>(f.set0->WithEpoch(3));
        }},
        {"anchor belongs to another epoch", [](auto& f) {
            f.State().epoch = 1;
            f.State().epoch_starts = {1, 11};
            f.State().previous = f.set0;
            f.State().current = std::make_shared<const node::ValidatorSetSnapshot>(f.set0->WithEpoch(1));
            f.State().next = std::make_shared<const node::ValidatorSetSnapshot>(f.set0->WithEpoch(2));
        }},
    };
    for (const auto& [label, mutate] : mutations) {
        Fixture f;
        node::FinalitySigner signer;
        f.Arm(signer);
        f.Approve(signer, f.pin);
        mutate(f);
        Check(f.Sign(signer, 31).empty(), label + " must refuse runtime recovery");
        Check(f.Journal() == f.original, label + " must preserve the original vote and lock");
    }
    Fixture f;
    node::FinalitySigner signer;
    f.Arm(signer);
    auto nonscheduled{f.pin};
    nonscheduled.anchor_height = 12;
    nonscheduled.anchor_block_hash = f.hashes[12];
    f.Approve(signer, nonscheduled); // setter checks journal facts, not chain observations
    Check(f.Sign(signer, 32).empty(), "anchor20 deep but off schedule must refuse");
    Check(f.Journal() == f.original, "off-schedule anchor cannot move journal");
}

void PreviousEpochRemainsEligible()
{
    Fixture f;
    node::FinalitySigner signer;
    f.Arm(signer);
    f.Approve(signer, f.pin);
    f.State().epoch = 1;
    f.State().epoch_starts = {1, 31};
    f.State().previous = f.set0;
    f.State().current = std::make_shared<const node::ValidatorSetSnapshot>(f.set0->WithEpoch(1));
    f.State().next = std::make_shared<const node::ValidatorSetSnapshot>(f.set0->WithEpoch(2));
    f.State().handover_certified = true;
    Check(f.Sign(signer, 31).empty(), "retained previous-epoch incident can move to its selected anchor");
    f.CheckAnchorOnly();
}

void ApprovalIsEphemeralAndFailureDoesNotReplaceIt()
{
    Fixture f;
    node::FinalitySigner signer;
    f.Arm(signer);
    f.Approve(signer, f.pin);
    f.Arm(signer); // successful key/journal reload discards the previous local approval
    Check(f.Sign(signer, 31).empty(), "key reload must clear unconsumed operator approval");
    Check(f.Journal() == f.original, "key reload cannot activate old approval");
    f.Approve(signer, f.pin);
    node::FinalitySigner restarted;
    f.Arm(restarted);
    Check(f.Sign(restarted, 31).empty(), "unconsumed approval is not stored for restart");
    Check(f.Journal() == f.original, "restart without explicit approval remains locked");
    auto invalid{f.pin};
    invalid.validator_key = f.other_validator;
    std::string error;
    Check(!signer.SetOperatorTrustedRecovery(invalid, error), "invalid replacement is rejected");
    Check(f.Sign(signer, 31).empty(), "original explicitly accepted plan survives rejected replacement");
    f.CheckAnchorOnly();
}

void SelectedAnchorOrphanedAgain()
{
    Fixture f;
    node::FinalitySigner signer;
    f.Arm(signer);
    f.Approve(signer, f.pin);
    Check(f.Sign(signer, 31).empty(), "prepare selected anchor without a later vote");
    f.CheckAnchorOnly();
    const auto anchored{f.Journal()};
    f.hashes[11] = NonzeroHash(f.rng);
    Check(f.Sign(signer, 33).empty(), "orphaned selected anchor cannot reuse old approval");
    Check(f.Journal() == anchored, "second fork must retain the selected ancestry lock");
    std::string error;
    Check(!signer.SetOperatorTrustedRecovery(f.pin, error), "old incident no longer exactly matches moved journal");
    node::FinalitySigner restarted;
    f.Arm(restarted);
    Check(!restarted.SetOperatorTrustedRecovery(f.pin, error), "restart cannot re-approve old incident after anchor move");
    Check(f.Sign(restarted, 33).empty(), "restart remains locked on orphaned selected anchor");
    Check(f.Journal() == anchored, "restart cannot roll back moved ancestry lock");
}

void Run(const char* name, const std::function<void()>& body)
{
    body();
    ++cases;
    std::cout << "PASS: " << name << '\n';
}
} // namespace

int main(int argc, char**)
{
    if (argc != 1) {
        std::cerr << "This isolated test accepts no arguments or data-directory paths.\n";
        return 2;
    }
    try {
        RandomInit();
        Run("no approval and unchanged hardened recovery", NoApprovalAndHardenedCompatibility);
        Run("operator19/20 boundary, preserved vote, restart and next signature", OperatorDepthPreservationAndNextVote);
        Run("stronger consensus depth", StrongerConsensusDepth);
        Run("mainnet new-vote20 and unchanged receiver12", MainnetSigningPolicyDoesNotRaiseReceiverDepth);
        Run("setter identity/domain/incident/set guards", SetterRejectsDifferentJournalFacts);
        Run("loaded durable journal required", SetterRequiresLoadedPersistentJournal);
        Run("runtime chain, schedule and lineage guards", RuntimeChainAndLineageRefusals);
        Run("retained previous epoch", PreviousEpochRemainsEligible);
        Run("ephemeral approval and invalid replacement", ApprovalIsEphemeralAndFailureDoesNotReplaceIt);
        Run("selected anchor orphaned again", SelectedAnchorOrphanedAgain);
        std::cout << "PASS: " << cases << " actual-signer cases, " << checks
                  << " checks; randomized synthetic data only.\n"
                  << "No mining, wallet, network, mainnet pin, tracker derivation or block validation was exercised.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << cases << " cases and " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
