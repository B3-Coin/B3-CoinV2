// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// Standalone tests of the PRODUCTION durable store, not a copy of its logic.
// No node, chainstate, wallet, network, mainnet constants, or mining fixture.
// See README-targeted-recovery-tests.md for build and coverage boundaries.

#include <node/finality_signer_store.h>
#include <random.h>
#include <util/translation.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

// Standalone utility linkage needs the application's translation hook; this
// test has no UI and deliberately leaves translation disabled.
const TranslateFn G_TRANSLATION_FUN{nullptr};

namespace {
size_t checks{0};

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

// Own only a fresh, unpredictably named directory below the system temporary
// directory. There is deliberately no command-line data-directory option.
class TemporaryDirectory
{
    fs::path m_path;

public:
    explicit TemporaryDirectory(FastRandomContext& rng)
    {
        const fs::path parent{std::filesystem::temp_directory_path()};
        for (int attempt{0}; attempt < 16; ++attempt) {
            const fs::path candidate{parent / fs::u8path(
                "b3-targeted-recovery-test-" + NonzeroHash(rng).GetHex())};
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
        // m_path was assigned only after this process created that exact path.
        std::filesystem::remove_all(m_path.std_path(), error);
        if (error) std::cerr << "Could not remove synthetic test directory: "
                             << fs::PathToString(m_path) << '\n';
    }

    const fs::path& Path() const { return m_path; }
};

struct Fixture
{
    FastRandomContext rng;
    TemporaryDirectory directory{rng};
    Consensus::FinalitySignerRecovery pin;
    modern::ValidatorKeyBytes validator{};
    uint256 vote_digest{NonzeroHash(rng)};
    uint256 anchor_digest{NonzeroHash(rng)};

    Fixture()
    {
        const auto identity{NonzeroHash(rng)};
        std::copy(identity.begin(), identity.end(), validator.begin());
        pin.validator_key = validator;
        pin.chain_domain = NonzeroHash(rng);
        pin.incident_height = 100;
        pin.incident_block_hash = NonzeroHash(rng);
        pin.incident_epoch = 2;
        pin.incident_signing_set_hash = NonzeroHash(rng);
        pin.incident_successor_set_hash = NonzeroHash(rng);
        pin.anchor_height = 120;
        do { pin.anchor_block_hash = NonzeroHash(rng); }
        while (pin.anchor_block_hash == pin.incident_block_hash);
        Check(pin.Valid(), "synthetic targeted pin must be structurally valid");
    }

    node::FinalitySignerStore Fresh(const char* name)
    {
        node::FinalitySignerStore store;
        std::string error;
        Check(store.Open(directory.Path() / name, pin.chain_domain, validator, error),
              std::string{name} + ": open fresh store: " + error);
        Check(store.IsAbsent(), "new test store must be absent");
        Check(store.InitializeEmpty(error), "initialize synthetic marker: " + error);
        Check(store.CommitSignedCheckpoint(pin.incident_height, pin.incident_block_hash,
                  vote_digest, pin.incident_epoch, pin.incident_signing_set_hash,
                  pin.incident_successor_set_hash, error), "record synthetic vote: " + error);
        return store;
    }

    node::FinalitySignerState Reload(const node::FinalitySignerStore& store)
    {
        node::FinalitySignerStore restarted;
        std::string error;
        Check(restarted.Open(store.Path().parent_path(), pin.chain_domain, validator, error),
              "reload synthetic store: " + error);
        Check(restarted.State().has_value(), "reloaded journal must have a record");
        return *restarted.State();
    }
};

void ExactRecoveryAndRejections(Fixture& fixture)
{
    auto store{fixture.Fresh("exact")};
    const auto before{*store.State()};
    const auto refused = [&](auto mutate, const char* label) {
        auto bad{fixture.pin};
        mutate(bad);
        std::string error;
        Check(!store.CommitPinnedRecoveryAnchor(bad, fixture.anchor_digest, error), label);
        Check(!error.empty(), "rejection must explain its failure");
        Check(*store.State() == before, "rejection must not change in-memory state");
        Check(fixture.Reload(store) == before, "rejection must not change durable state");
    };
    refused([](auto& pin) { (*pin.validator_key)[0] ^= 1; }, "wrong validator must fail");
    refused([](auto& pin) { pin.validator_key->fill(0); }, "zero target must fail");
    refused([&](auto& pin) { pin.chain_domain = NonzeroHash(fixture.rng); }, "wrong domain must fail");
    refused([](auto& pin) { --pin.incident_height; }, "wrong incident height must fail");
    refused([&](auto& pin) { pin.incident_block_hash = NonzeroHash(fixture.rng); }, "wrong incident hash must fail");
    refused([](auto& pin) { ++pin.incident_epoch; }, "wrong epoch must fail");
    refused([&](auto& pin) { pin.incident_signing_set_hash = NonzeroHash(fixture.rng); }, "wrong signing set must fail");
    refused([&](auto& pin) { pin.incident_successor_set_hash = NonzeroHash(fixture.rng); }, "wrong successor must fail");
    refused([](auto& pin) { pin.anchor_height = pin.incident_height; }, "equal-height anchor must fail");
    refused([](auto& pin) { pin.anchor_height = pin.incident_height - 1; }, "backward anchor must fail");
    refused([](auto& pin) { pin.anchor_block_hash.SetNull(); }, "null anchor must fail");
    refused([](auto& pin) { pin.anchor_block_hash = pin.incident_block_hash; }, "incident is not a new anchor");
    std::string error;
    Check(!store.CommitPinnedRecoveryAnchor(fixture.pin, uint256{}, error), "null anchor digest must fail");
    Check(fixture.Reload(store) == before, "null digest must preserve durable state");

    Check(store.CommitPinnedRecoveryAnchor(fixture.pin, fixture.anchor_digest, error),
          "exact targeted incident must recover: " + error);
    auto expected{before};
    expected.lock_height = fixture.pin.anchor_height;
    expected.lock_block_hash = fixture.pin.anchor_block_hash;
    expected.lock_digest = fixture.anchor_digest;
    Check(*store.State() == expected, "ONLY ancestry height/hash/digest may change; retain original vote/identity/sets");
    Check(fixture.Reload(store) == expected, "restart must retain moved lock and original vote");
    Check(!store.CommitPinnedRecoveryAnchor(fixture.pin, fixture.anchor_digest, error), "recovery is one-time only");
    Check(!store.CommitSignedCheckpoint(110, NonzeroHash(fixture.rng), NonzeroHash(fixture.rng),
              fixture.pin.incident_epoch, fixture.pin.incident_signing_set_hash,
              fixture.pin.incident_successor_set_hash, error), "new vote below anchor must fail");
    Check(!store.CommitSignedCheckpoint(120, fixture.pin.anchor_block_hash, fixture.anchor_digest,
              fixture.pin.incident_epoch, fixture.pin.incident_signing_set_hash,
              fixture.pin.incident_successor_set_hash, error), "new vote at anchor must fail");
    Check(!store.CommitSignedCheckpoint(100, fixture.pin.incident_block_hash, NonzeroHash(fixture.rng),
              fixture.pin.incident_epoch, fixture.pin.incident_signing_set_hash,
              fixture.pin.incident_successor_set_hash, error), "competing old-height digest must fail");
    // Exact replay of an already recorded vote is idempotent, not a new vote,
    // and must not roll the later ancestry anchor backward.
    Check(store.CommitSignedCheckpoint(100, fixture.pin.incident_block_hash, fixture.vote_digest,
              fixture.pin.incident_epoch, fixture.pin.incident_signing_set_hash,
              fixture.pin.incident_successor_set_hash, error), "exact old vote may be idempotent");
    Check(fixture.Reload(store) == expected, "idempotent old vote must not roll back anchor");
    Check(store.CommitSignedCheckpoint(130, NonzeroHash(fixture.rng), NonzeroHash(fixture.rng),
              fixture.pin.incident_epoch, fixture.pin.incident_signing_set_hash,
              fixture.pin.incident_successor_set_hash, error), "new vote above anchor must work");
    Check(store.State()->last_signed_height == 130 && store.State()->lock_height == 130,
          "new vote must advance both watermarks");
    Check(fixture.Reload(store) == *store.State(), "new vote must survive restart");
}

void DifferentHistoriesAndStaleWriter(Fixture& fixture)
{
    std::string error;
    auto newer{fixture.Fresh("newer-vote")};
    Check(newer.CommitSignedCheckpoint(110, NonzeroHash(fixture.rng), NonzeroHash(fixture.rng),
              fixture.pin.incident_epoch, fixture.pin.incident_signing_set_hash,
              fixture.pin.incident_successor_set_hash, error), "prepare newer recorded vote");
    const auto newer_state{*newer.State()};
    Check(!newer.CommitPinnedRecoveryAnchor(fixture.pin, fixture.anchor_digest, error), "newer vote is not pinned incident");
    Check(fixture.Reload(newer) == newer_state, "recovery must not replace a newer vote");

    auto moved{fixture.Fresh("different-lock")};
    Check(moved.CommitCertifiedAnchor(110, NonzeroHash(fixture.rng), NonzeroHash(fixture.rng),
              fixture.pin.incident_epoch, fixture.pin.incident_signing_set_hash,
              fixture.pin.incident_successor_set_hash, error), "prepare already moved lock");
    const auto moved_state{*moved.State()};
    Check(!moved.CommitPinnedRecoveryAnchor(fixture.pin, fixture.anchor_digest, error), "different existing lock must fail");
    Check(fixture.Reload(moved) == moved_state, "different lock must remain intact");

    auto writer{fixture.Fresh("stale-writer")};
    node::FinalitySignerStore stale;
    Check(stale.Open(writer.Path().parent_path(), fixture.pin.chain_domain, fixture.validator, error),
          "open second synthetic store handle before advancement");
    const auto stale_state{*stale.State()};
    Check(writer.CommitSignedCheckpoint(130, NonzeroHash(fixture.rng), NonzeroHash(fixture.rng),
              fixture.pin.incident_epoch, fixture.pin.incident_signing_set_hash,
              fixture.pin.incident_successor_set_hash, error), "advance first synthetic writer");
    Check(!stale.CommitPinnedRecoveryAnchor(fixture.pin, fixture.anchor_digest, error),
          "stale in-memory incident must not overwrite newer disk record");
    Check(*stale.State() == stale_state, "failed stale writer must retain its in-memory record");
    Check(fixture.Reload(writer) == *writer.State(), "stale attempt must not roll back newer disk record");

    auto legacy{fixture.Fresh("untargeted-compatibility")};
    auto untargeted{fixture.pin};
    untargeted.validator_key.reset();
    Check(legacy.CommitPinnedRecoveryAnchor(untargeted, fixture.anchor_digest, error),
          "pre-existing untargeted incident semantics must remain supported");
}

void MissingCorruptAndForeignJournals(Fixture& fixture)
{
    std::string error;
    auto deleted{fixture.Fresh("deleted")};
    const auto prior{*deleted.State()};
    Check(std::filesystem::remove(deleted.Path().std_path()), "delete only this synthetic journal");
    Check(!deleted.CommitPinnedRecoveryAnchor(fixture.pin, fixture.anchor_digest, error),
          "deleted predecessor must refuse recovery");
    Check(*deleted.State() == prior, "failed recovery must preserve in-memory predecessor");
    Check(!std::filesystem::exists(deleted.Path().std_path()), "recovery must not recreate a deleted journal");
    node::FinalitySignerStore absent;
    Check(absent.Open(deleted.Path().parent_path(), fixture.pin.chain_domain, fixture.validator, error),
          "open distinguishes absent journal from corrupt state");
    Check(absent.IsAbsent(), "deleted journal remains explicitly absent after restart");
    Check(!absent.CommitPinnedRecoveryAnchor(fixture.pin, fixture.anchor_digest, error),
          "restarted absent store cannot use recovery pin");
    Check(!absent.CommitSignedCheckpoint(130, NonzeroHash(fixture.rng), NonzeroHash(fixture.rng),
              fixture.pin.incident_epoch, fixture.pin.incident_signing_set_hash,
              fixture.pin.incident_successor_set_hash, error), "uninitialized absent store cannot record a vote");
    // Do not call InitializeEmpty here: deciding whether an absent journal may
    // be initialized is the signer's chain-dependent policy, not this test.

    auto corrupt{fixture.Fresh("corrupt")};
    {
        std::fstream file{corrupt.Path().std_path(), std::ios::binary | std::ios::in | std::ios::out};
        Check(file.is_open(), "open only synthetic journal for corruption injection");
        char byte{};
        file.read(&byte, 1);
        Check(file.good(), "read synthetic magic byte");
        byte ^= 1;
        file.seekp(0);
        file.write(&byte, 1);
        file.close();
        Check(!file.fail(), "flush synthetic corruption");
    }
    Check(!corrupt.CommitPinnedRecoveryAnchor(fixture.pin, fixture.anchor_digest, error),
          "corrupt predecessor must refuse recovery");
    node::FinalitySignerStore corrupt_restart;
    Check(!corrupt_restart.Open(corrupt.Path().parent_path(), fixture.pin.chain_domain,
                               fixture.validator, error), "corrupt journal must fail closed on restart");

    auto original{fixture.Fresh("foreign")};
    auto other_validator{fixture.validator};
    other_validator[0] ^= 1;
    const auto foreign_path{node::FinalitySignerStore::StatePath(
        original.Path().parent_path(), fixture.pin.chain_domain, other_validator)};
    Check(std::filesystem::copy_file(original.Path().std_path(), foreign_path.std_path()),
          "copy synthetic record to a distinct synthetic identity path");
    node::FinalitySignerStore foreign;
    Check(!foreign.Open(original.Path().parent_path(), fixture.pin.chain_domain, other_validator, error),
          "foreign journal contents must not become another validator's state");
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
        Fixture fixture;
        ExactRecoveryAndRejections(fixture);
        DifferentHistoriesAndStaleWriter(fixture);
        MissingCorruptAndForeignJournals(fixture);
        std::cout << "PASS: " << checks << " production-store checks; randomized synthetic data only.\n"
                  << "No chain/anchor validation, BLS signing, mining, wallet, or network was exercised.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
