// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/storage/seat_signer.h>

#include <flowmesh2/storage/journal.h>

#include <optional>
#include <utility>

namespace flowmesh2::storage {

// The secret key lives only here (guard rule 7).
struct SeatSigner::Key {
    bls::SecretKey secret;
    bls::PublicKey pub;
};

SeatSigner::SeatSigner(std::unique_ptr<const Key> key, SignerLog& log) : m_key{std::move(key)}, m_log{&log} {}
SeatSigner::SeatSigner(SeatSigner&&) noexcept = default;
SeatSigner& SeatSigner::operator=(SeatSigner&&) noexcept = default;
SeatSigner::~SeatSigner() = default;

Result<SeatSigner> SeatSigner::FromIKM(std::span<const unsigned char> ikm, SignerLog& log)
{
    if (ikm.size() < 32) return Error{ErrorCode::INVALID_INPUT, "BLS input keying material shorter than 32 bytes"};
    std::optional<bls::SecretKey> secret{bls::SecretKey::FromIKM(ikm)};
    if (!secret.has_value()) return Error{ErrorCode::INVALID_INPUT, "BLS KeyGen gave no key"};
    const bls::PublicKey pub{secret->GetPublicKey()};
    return Ok(SeatSigner{std::unique_ptr<const Key>{new Key{*secret, pub}}, log});
}

const bls::PublicKey& SeatSigner::PublicKey() const { return m_key->pub; }

VoteSignature SeatSigner::ProofOfPossession() const { return m_key->secret.SignPoP().Compressed(); }

Result<VoteSignature> SeatSigner::Sign(const DurableIntent& intent) const
{
    if (m_log->IsFatal()) return Error{ErrorCode::FENCED, JOURNAL_FENCED};
    if (!m_log->Knows(intent)) return Error{ErrorCode::VERIFY_FAILED, INTENT_UNKNOWN};
    const uint256& digest{intent.Digest()};
    const VoteSignature signature{m_key->secret.Sign(std::span<const unsigned char>{digest.data(), digest.size()}).Compressed()};
    if (m_observer != nullptr) m_observer->OnSigned(intent.Slot(), intent.View(), digest);
    return Ok(signature);
}

Result<DurableSignedVote> SeatSigner::SignDurably(const PendingIntent& intent, const std::function<Result<void>()>& before_sign)
{
    if (Result<uint64_t> barrier{m_log->Barrier()}; !barrier.has_value()) return barrier.error(); // B-INTENT
    Result<DurableIntent> durable{m_log->Confirm(intent)};
    if (!durable.has_value()) return durable.error();
    if (before_sign) {
        if (const Result<void> go{before_sign()}; !go.has_value()) return go.error(); // the intent stays durable, nothing is signed
    }
    Result<VoteSignature> signature{Sign(*durable)};
    if (!signature.has_value()) return signature.error();
    Result<PendingSignedVote> signed_vote{m_log->AppendSignedVote(*durable, *signature)};
    if (!signed_vote.has_value()) return signed_vote.error();
    if (Result<uint64_t> barrier{m_log->Barrier()}; !barrier.has_value()) return barrier.error(); // B-VOTE
    return m_log->Confirm(*signed_vote);
}

} // namespace flowmesh2::storage
