// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_STORAGE_SEAT_SIGNER_H
#define B3_FLOWMESH2_STORAGE_SEAT_SIGNER_H

#include <crypto/bls.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/storage/durable.h>
#include <flowmesh2/storage/signer_log.h>
#include <flowmesh2/storage/tokens.h>
#include <uint256.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

/** The seat's BLS signer (slice R8; ARCHITECTURE.md 7.2).
 *
 * SeatSigner::Sign(const DurableIntent&) is the ONLY path that BLS-signs a
 * vote. It takes nothing but a DurableIntent, a token that only
 * Journal::Confirm can mint after the INTENT record passed its barrier
 * (B-INTENT), so a vote can never be signed before its intent is durable;
 * the compile-fail control test/compile_fail/sign_without_intent.cpp proves
 * that a pending intent or a bare digest does not compile. It signs the
 * intent's digest (Durable::Digest(), for a COMMIT vote the header's
 * CommitDigest) under the BLS signature DST.
 *
 * Sign refuses with:
 * - FENCED (JOURNAL_FENCED) when the signer journal is fatal (no signing
 *   after a storage failure, plan section 3.7);
 * - VERIFY_FAILED (INTENT_UNKNOWN) for an intent that is not this signer
 *   log's (another journal instance, sequence number, key or digest).
 *
 * The secret key never appears in this header: it lives in seat_signer.cpp,
 * the only non-test, non-dev, non-sim file allowed to name bls::SecretKey or
 * call .Sign( (guard rule 7). ProofOfPossession signs the public key under
 * the PoP DST, which cannot be a vote; seat sets need it (SeatSet::FromPoP).
 *
 * Outline keys: FromIKM takes input keying material (at least 32 bytes) from
 * the caller. TEST and dev seats only; real key custody is not part of the
 * outline. A SeatSigner keeps a pointer to its SignerLog, which must outlive
 * it, and is owned by the sealing thread like the log.
 *
 * Observation (slice R14): SetSignObserver installs a SignObserver that sees
 * every vote signature Sign produced, after it exists and before Sign
 * returns; a refused Sign is not reported. The simulator's trace uses it to
 * record bls.sign (ARCHITECTURE.md 14.4 P3). It changes nothing about what
 * is signed or when: without an observer (the default) Sign behaves exactly
 * as in R8.
 */
namespace flowmesh2::storage {

/** Sees the vote signatures of one SeatSigner (see the file comment). The
 * callback gets the intent's slot, view and digest, never the key or the
 * token, and runs on the signing thread; it must not call the signer.
 */
class SignObserver
{
public:
    virtual ~SignObserver() = default;
    virtual void OnSigned(uint64_t slot, uint64_t view, const uint256& digest) = 0;
};

class SeatSigner
{
public:
    //! INVALID_INPUT if the IKM is shorter than 32 bytes or gives no key.
    static Result<SeatSigner> FromIKM(std::span<const unsigned char> ikm, const SignerLog& log);

    SeatSigner(SeatSigner&&) noexcept;
    SeatSigner& operator=(SeatSigner&&) noexcept;
    SeatSigner(const SeatSigner&) = delete;
    SeatSigner& operator=(const SeatSigner&) = delete;
    ~SeatSigner();

    const bls::PublicKey& PublicKey() const;
    //! The proof of possession of PublicKey() (POP_DST).
    VoteSignature ProofOfPossession() const;

    //! The only BLS vote signing path; see the file comment.
    Result<VoteSignature> Sign(const DurableIntent& intent) const;

    //! Report every successful Sign to `observer` (nullptr: none). The observer must outlive the signer.
    void SetSignObserver(SignObserver* observer) { m_observer = observer; }

private:
    struct Key;
    SeatSigner(std::unique_ptr<const Key> key, const SignerLog& log);

    std::unique_ptr<const Key> m_key;
    const SignerLog* m_log;
    SignObserver* m_observer{nullptr};
};

} // namespace flowmesh2::storage

#endif // B3_FLOWMESH2_STORAGE_SEAT_SIGNER_H
