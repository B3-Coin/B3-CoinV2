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
 */
namespace flowmesh2::storage {

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

private:
    struct Key;
    SeatSigner(std::unique_ptr<const Key> key, const SignerLog& log);

    std::unique_ptr<const Key> m_key;
    const SignerLog* m_log;
};

} // namespace flowmesh2::storage

#endif // B3_FLOWMESH2_STORAGE_SEAT_SIGNER_H
