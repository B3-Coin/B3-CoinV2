// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/storage/seat_signer.h>

#include <flowmesh2/storage/journal.h>
#include <flowmesh2/storage/control_log.h>

#include <optional>
#include <utility>

namespace flowmesh2::storage {

// The secret key lives only here (guard rule 7).
struct SeatSigner::Key {
    bls::SecretKey secret;
    bls::PublicKey pub;
};

SeatSigner::SeatSigner(std::unique_ptr<const Key> key, const SignerLog& log) : m_key{std::move(key)}, m_log{&log} {}
SeatSigner::SeatSigner(SeatSigner&&) noexcept = default;
SeatSigner& SeatSigner::operator=(SeatSigner&&) noexcept = default;
SeatSigner::~SeatSigner() = default;

Result<SeatSigner> SeatSigner::FromIKM(std::span<const unsigned char> ikm, const SignerLog& log)
{
    if (ikm.size() < 32) return Error{ErrorCode::INVALID_INPUT, "BLS input keying material shorter than 32 bytes"};
    std::optional<bls::SecretKey> secret{bls::SecretKey::FromIKM(ikm)};
    if (!secret.has_value()) return Error{ErrorCode::INVALID_INPUT, "BLS KeyGen gave no key"};
    const bls::PublicKey pub{secret->GetPublicKey()};
    return Ok(SeatSigner{std::unique_ptr<const Key>{new Key{*secret, pub}}, log});
}

Result<SeatSigner> SeatSigner::FromIKM(std::span<const unsigned char> ikm, const SignerLog& log, const ControlLog& control_log)
{
    auto signer{FromIKM(ikm, log)};
    if (!signer.has_value()) return signer.error();
    if (signer->PublicKey() != control_log.SignerKey()) return Error{ErrorCode::INVALID_INPUT, "CONTROL_SIGNER_KEY"};
    signer->m_control_log = &control_log;
    return signer;
}

const bls::PublicKey& SeatSigner::PublicKey() const { return m_key->pub; }

VoteSignature SeatSigner::ProofOfPossession() const { return m_key->secret.SignPoP().Compressed(); }

Result<VoteSignature> SeatSigner::Sign(const DurableIntent& intent) const
{
    if (m_log->IsFatal()) return Error{ErrorCode::FENCED, JOURNAL_FENCED};
    if (!m_log->Knows(intent)) return Error{ErrorCode::VERIFY_FAILED, INTENT_UNKNOWN};
    if (m_control_log) {
        const auto& key{intent.Binding().key};
        if (auto allowed{m_control_log->AllowsVote(key.domain, key.epoch, key.slot, key.view)}; !allowed.has_value()) return allowed.error();
    }
    const uint256& digest{intent.Digest()};
    const VoteSignature signature{m_key->secret.Sign(std::span<const unsigned char>{digest.data(), digest.size()}).Compressed()};
    if (m_observer != nullptr) m_observer->OnSigned(intent.Slot(), intent.View(), digest);
    return Ok(signature);
}

Result<DurableSignedVote> SeatSigner::SignAndPersist(const DurableIntent& intent, SignerLog& log) const
{
    if (&log != m_log) return Error{ErrorCode::VERIFY_FAILED, INTENT_UNKNOWN};
    const auto signature{Sign(intent)};
    if (!signature.has_value()) return signature.error();
    const auto signed_vote{log.AppendSignedVote(intent, *signature)};
    if (!signed_vote.has_value()) return signed_vote.error();
    if (const auto barrier{log.Barrier()}; !barrier.has_value()) return barrier.error();
    return log.Confirm(*signed_vote);
}

Result<VoteSignature> SeatSigner::SignControl(const DurableControlIntent& intent) const
{
    if (m_log->IsFatal() || !m_control_log || m_control_log->IsFatal()) return Error{ErrorCode::FENCED, "CONTROL_SIGNING_UNAVAILABLE"};
    if (!m_control_log->Knows(intent)) return Error{ErrorCode::VERIFY_FAILED, "CONTROL_INTENT_UNKNOWN"};
    if (auto allowed{m_control_log->MaySign(intent)}; !allowed.has_value()) return allowed.error();
    const auto& digest{intent.Digest()};
    const VoteSignature signature{m_key->secret.Sign(std::span<const unsigned char>{digest.data(), digest.size()}).Compressed()};
    if (m_observer) m_observer->OnSigned(intent.Slot(), intent.View(), digest);
    return Ok(signature);
}

Result<DurableSignedControl> SeatSigner::SignControlAndPersist(const DurableControlIntent& intent, ControlLog& log) const
{
    if (&log != m_control_log) return Error{ErrorCode::VERIFY_FAILED, "CONTROL_SIGNER_LOG"};
    if (m_log->IsFatal() || log.IsFatal()) return Error{ErrorCode::FENCED, "CONTROL_SIGNING_UNAVAILABLE"};
    if (!log.Knows(intent)) return Error{ErrorCode::VERIFY_FAILED, "CONTROL_INTENT_UNKNOWN"};
    if (const auto existing{log.SignedAt(intent.Binding().key)}) {
        if (auto barrier{log.Barrier()}; !barrier.has_value()) return barrier.error();
        return log.Confirm(*existing); // exact retained signature; no new signing
    }
    auto signature{SignControl(intent)};
    if (!signature.has_value()) return signature.error();
    auto pending{log.AppendSigned(intent, *signature)};
    if (!pending.has_value()) return pending.error();
    if (auto barrier{log.Barrier()}; !barrier.has_value()) return barrier.error();
    return log.Confirm(*pending);
}

} // namespace flowmesh2::storage
