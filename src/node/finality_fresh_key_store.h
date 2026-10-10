// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_NODE_FINALITY_FRESH_KEY_STORE_H
#define B3COIN_NODE_FINALITY_FRESH_KEY_STORE_H

#include <consensus/params.h>
#include <crypto/bls.h>
#include <modern/finality_key.h>
#include <support/allocators/secure.h>
#include <sync.h>
#include <uint256.h>
#include <util/fs.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace node {

struct FreshLineageState;

/**
 * The finality signer directory under the network data directory. init.cpp
 * hands args.GetDataDirNet() / FINALITY_SIGNER_DIRNAME to the staking loop
 * and interfaces::Chain::freshFinalityKeyStatus reports the same path to the
 * wallet; the V1 and fresh-lineage signer journals and the fresh key store
 * all live there.
 */
inline constexpr const char* FINALITY_SIGNER_DIRNAME{"finality_signer"};

/**
 * B-37 fresh key store (b3-bridge-v2-finality-dependency.md section 6.4;
 * node changes section 7.7): the secret halves of a validator's per-epoch
 * fresh-lineage BLS keys, kept OUTSIDE the wallet file.
 *
 * Every key is generated from 32 bytes of GetStrongRandBytes() through the
 * IETF KeyGen (bls::SecretKey::FromIKM). Nothing here takes, reads or derives
 * from the wallet's identity secret, so a wallet backup can never regenerate
 * a fresh key (unlike V1's DeriveFinalityBlsKey). A key that is no longer
 * needed is ERASED: its secret is zeroed and the record keeps only public
 * data (FreshKeyErasable below is the signer policy deciding when).
 *
 * One file per (chain domain, validator key), next to the signer journals:
 *   <datadir>/<net>/finality_signer/
 *       finality_fresh_keys_<chain_domain GetHex>_<validator key hex>.dat
 *
 * Layout (integers little-endian; 77-byte header, 121-byte records):
 *   magic        "B3FKEYS1"                         8
 *   version      u8 = 1                             1
 *   chain_domain uint256 bytes                      32
 *   validator    x-only validator key               32
 *   count        u32, <= MAX_RECORDS                4
 *   count x {
 *     target_epoch u64                              8
 *     state        u8 (1 HELD, 2 ERASED)            1
 *     bls_pubkey   compressed G1                    48
 *     secret       big-endian scalar; all zero      32
 *                  when ERASED
 *     bind_txid    uint256 bytes; null until set    32
 *   }
 *   checksum     SHA256 (single) of every byte above 32
 *
 * Open() treats an absent file as an empty store and fails closed on
 * anything else that does not verify: wrong magic or version, checksum
 * mismatch, wrong length, count above MAX_RECORDS, another chain domain or
 * validator, an unknown state, a non-canonical public key, a duplicate
 * public key, a HELD secret that is invalid or does not match its public
 * key, or an ERASED record with any nonzero secret byte. Records keep their
 * creation order.
 *
 * Every mutation (Generate, SetBindTxid, Erase) runs under one process-wide
 * mutex, re-reads the file and refuses unless it still equals this
 * instance's view (absent if this instance found it absent), then writes a
 * temporary file created owner-only (0600 where POSIX permissions exist),
 * FileCommit, RenameOver and DirectoryCommit, as FinalitySignerStore does.
 * A stale instance (another one wrote meanwhile) must be reopened. An
 * instance itself is not thread-safe. Files are read and written unbuffered
 * through buffers that are cleansed when freed; in memory, secrets live in
 * locked memory (SecretMap).
 *
 * Records are never removed, so a store that reaches MAX_RECORDS refuses
 * Generate (one key per epoch is about eleven years of 1,440-block epochs at
 * 60 s; erased records hold only public data and could be compacted later).
 *
 * Erasure is logical: after Erase the store file holds no byte of the
 * secret. Blocks of the replaced file that the filesystem has not yet
 * reused (or keeps in a snapshot) are outside the node's control.
 */
enum class FreshKeyState : uint8_t {
    HELD = 1,
    ERASED = 2,
};

//! "held" / "erased" (RPC spelling).
const char* FreshKeyStateName(FreshKeyState state);

//! One fresh key's public record. The secret leaves the store only through
//! FreshKeyStore::SecretFor() (and Generate()'s return value).
struct FreshKeyRecord {
    uint64_t target_epoch{0};
    FreshKeyState state{FreshKeyState::HELD};
    modern::BlsPubkeyBytes bls_pubkey{};
    //! The binding transaction (SetBindTxid); null until recorded.
    uint256 bind_txid{};

    bool Held() const { return state == FreshKeyState::HELD; }
    friend bool operator==(const FreshKeyRecord&, const FreshKeyRecord&) = default;
};

class FreshKeyStore
{
public:
    using SecretBytes = std::array<unsigned char, bls::SECRET_SIZE>;
    //! Secrets by public key. Every node is a small separate allocation in
    //! locked memory, cleansed when freed (one contiguous secure buffer
    //! would exceed the locked pool's arena size for a large store).
    using SecretMap = std::map<modern::BlsPubkeyBytes, SecretBytes, std::less<modern::BlsPubkeyBytes>,
                               secure_allocator<std::pair<const modern::BlsPubkeyBytes, SecretBytes>>>;

private:
    //! The in-memory view: every record in creation order, and the secret of
    //! exactly the HELD ones.
    struct Content {
        std::vector<FreshKeyRecord> records;
        SecretMap secrets;
        friend bool operator==(const Content&, const Content&) = default;
    };

public:
    static constexpr size_t MAX_RECORDS{4096};
    static constexpr size_t HEADER_SIZE{8 + 1 + 32 + 32 + 4};
    static constexpr size_t RECORD_SIZE{8 + 1 + bls::PUBKEY_SIZE + bls::SECRET_SIZE + 32};
    static constexpr size_t CHECKSUM_SIZE{32};
    static constexpr uint8_t VERSION{1};
    static constexpr std::array<unsigned char, 8> MAGIC{'B', '3', 'F', 'K', 'E', 'Y', 'S', '1'};

    FreshKeyStore() = default;
    FreshKeyStore(const FreshKeyStore&) = delete;
    FreshKeyStore& operator=(const FreshKeyStore&) = delete;

    //! The store file of (`chain_domain`, `validator_key`) in `directory`.
    static fs::path StatePath(const fs::path& directory, const uint256& chain_domain,
                              const modern::ValidatorKeyBytes& validator_key);

    /** Load the store of (`chain_domain`, `validator_key`) from `directory`.
     * An absent file is an empty store (nothing is created until the first
     * mutation); an unverifiable file fails closed and leaves this instance
     * closed. Reopening discards the previous view. */
    bool Open(const fs::path& directory, const uint256& chain_domain,
              const modern::ValidatorKeyBytes& validator_key, std::string& error) EXCLUSIVE_LOCKS_REQUIRED(!s_mutex);

    bool IsOpen() const { return m_open; }
    //! Open, and no store file existed when last read or written.
    bool IsAbsent() const { return m_open && !m_on_disk; }
    const fs::path& Path() const { return m_path; }
    const uint256& ChainDomain() const { return m_chain_domain; }
    const modern::ValidatorKeyBytes& ValidatorKey() const { return m_validator_key; }

    //! Every record, in creation order (public data only).
    std::vector<FreshKeyRecord> Records() const;
    std::optional<FreshKeyRecord> RecordFor(const modern::BlsPubkeyBytes& bls_pubkey) const;
    //! True when a HELD key names `target_epoch`.
    bool HoldsKeyForEpoch(uint64_t target_epoch) const;

    /** Generate, persist and return a new random key for `target_epoch`.
     * Durable (fsynced and renamed into place) before it returns. Refuses
     * when a HELD key already names that epoch (binding a second key would
     * void the seat, finality dependency 6.4; erase it first to replace it)
     * and when the store holds MAX_RECORDS records. */
    std::optional<bls::SecretKey> Generate(uint64_t target_epoch, std::string& error) EXCLUSIVE_LOCKS_REQUIRED(!s_mutex);

    /** Record the transaction that binds `bls_pubkey`. Idempotent for the
     * same txid; refuses a null txid, an unknown or ERASED key, and a
     * different txid (a key is never bound twice). */
    bool SetBindTxid(const modern::BlsPubkeyBytes& bls_pubkey, const uint256& txid, std::string& error)
        EXCLUSIVE_LOCKS_REQUIRED(!s_mutex);

    /** Erase the secret of `bls_pubkey` for good: zeroed, state ERASED,
     * durable. Idempotent for an already erased key; refuses an unknown key. */
    bool Erase(const modern::BlsPubkeyBytes& bls_pubkey, std::string& error) EXCLUSIVE_LOCKS_REQUIRED(!s_mutex);

    //! The secret of a HELD key; nullopt for an unknown or ERASED key.
    std::optional<bls::SecretKey> SecretFor(const modern::BlsPubkeyBytes& bls_pubkey) const;

private:
    //! Index of `bls_pubkey` in m_content.records.
    std::optional<size_t> Find(const modern::BlsPubkeyBytes& bls_pubkey) const;
    //! Verify the on-disk predecessor, then durably replace it with `next`.
    bool Commit(const Content& next, std::string& error) EXCLUSIVE_LOCKS_REQUIRED(s_mutex);

    //! Serializes every load and mutation of every store in this process, so
    //! two instances of one validator never interleave a re-read and a write.
    static GlobalMutex s_mutex;

    bool m_open{false};
    bool m_on_disk{false};
    fs::path m_path;
    uint256 m_chain_domain{};
    modern::ValidatorKeyBytes m_validator_key{};
    Content m_content;
};

/**
 * Signer erasure policy (pure; finality dependency section 6.4 "Erasure and
 * its limit"). FreshKeyRetiredSince returns the height from which the
 * erasure depth of a fresh key of target epoch `target_epoch` is counted on
 * the chain whose fresh state is `s`, or nullopt to keep it:
 *   - nullopt unless the lineage is bootstrapped (s.epoch, s.epoch_starts,
 *     s.current and s.next are meaningful),
 *   - nullopt for target_epoch > s.epoch + 1: a key for a later epoch is
 *     kept (Set'_{e+1} is built when epoch e begins, later ones are open),
 *   - nullopt while Set'_e (current) or Set'_{e+1} (next) seats the key: it
 *     can still sign what a current or coming committee needs, so a
 *     carried-over key stays,
 *   - start_{e-1} when Set'_{e-1} (previous) does not seat it either and
 *     target_epoch <= e: its committee was fixed by start_{e-1} and its last
 *     seat, if any, is in an epoch <= e-2, whose handover out was included
 *     below start_{e-1},
 *   - start_e otherwise (last seated in e-1, whose handover out was included
 *     below start_e; or a key for Set'_{e+1}, fixed at start_e, that it does
 *     not seat).
 * FreshKeyErasable holds exactly when that height is `horizon` (>= 1)
 * blocks deep: tip_height - since >= horizon. So a key is erased once the
 * handover out of its last seated epoch is certified on B3 and more than
 * `horizon` blocks deep, and a key that was never seated (no counted
 * binding, a void seat, a skipped carry-over epoch) once its committee is
 * fixed and deep. Counting from the start of the epoch that retired the key
 * (not from the current epoch's start) keeps erasure possible on a timely
 * chain whose horizon is not below the epoch length E (mainnet: E = horizon
 * = 1,440). No verifier condition applies: a lagging verifier catches up
 * with certificates already on B3.
 */
std::optional<int> FreshKeyRetiredSince(const FreshLineageState& s, const modern::BlsPubkeyBytes& bls_pubkey,
                                        uint64_t target_epoch);
bool FreshKeyErasable(const FreshLineageState& s, const modern::BlsPubkeyBytes& bls_pubkey, uint64_t target_epoch,
                      int tip_height, int horizon);

/**
 * The erasure depth: the modern reorg horizon (the depth below which a
 * reorg is refused; 1,440 on mainnet, regtest -b3reorghorizon), else the
 * finality epoch length E. INT_MAX (never erasable) without modern PoS.
 */
int FreshKeyErasureHorizon(const Consensus::Params& params);

} // namespace node

#endif // B3COIN_NODE_FINALITY_FRESH_KEY_STORE_H
