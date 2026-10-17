// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_SIM_SIM_CLIENT_H
#define B3_FLOWMESH2_SIM_SIM_CLIENT_H

#include <flowmesh2/core/account_leaf.h>
#include <flowmesh2/core/action.h>
#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/market.h>
#include <flowmesh2/core/outcome.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/seat_set.h>
#include <flowmesh2/net/client_api.h>
#include <flowmesh2/read/read_service.h>
#include <key.h>
#include <pubkey.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/** A simulated client (slice R14; ARCHITECTURE.md 14.1, 14.4 P2-P5 and
 * 14.5; [CC] ownership boundary 7).
 *
 * Keys. A TEST BIP340 key whose secret is SHA256("fm2/sim/<name>"); never a
 * real key. Signing needs an ECC_Context (the fm2-sim CLI and the test
 * runner hold one). The aux randomness of each signature is
 * SHA256("fm2/sim/aux/<name>/<label>"), so the signed bytes, and with them
 * every actions_root, are the same for every seed.
 *
 * Outbox. Every action is signed ONCE, when it is created under its label,
 * and kept as exact bytes. Submitting and resubmitting send those bytes;
 * the client never re-signs (a second SignOnce of a label is refused with
 * INVALID_INPUT CLIENT_LABEL_TAKEN). TamperedSignature returns a copy with
 * the first signature byte flipped (N1): bytes the client never signed,
 * never stored.
 *
 * Verification is engine-off: only verify/b3verify against the client's
 * trust inputs (the trusted seat set, the profile and the markets it
 * trades), plus field comparisons of what a reply claims against the
 * certified header. VerifyHead checks the certificate (VerifyCertificate),
 * the header's domain and profile hash, and that the reply's commit
 * identity, scope and label are the header's (one lease: P4).
 * VerifyAccount, VerifyIncluded and VerifyExpiredReply verify an account
 * leaf, an included outcome and an EXPIRED proof the same way. Each
 * failure is VERIFY_FAILED (or b3verify's own detail) and never trusted.
 */
namespace flowmesh2::sim {

// Static details (ErrorCode in parentheses).
inline constexpr std::string_view CLIENT_KEY{"the client key label gives no key"};                  //!< (INVALID_INPUT)
inline constexpr std::string_view CLIENT_LABEL_TAKEN{"the outbox already holds this label"};        //!< (INVALID_INPUT)
inline constexpr std::string_view CLIENT_LABEL_UNKNOWN{"no outbox entry with this label"};          //!< (INVALID_INPUT)
inline constexpr std::string_view CLIENT_SIGN_FAILED{"BIP340 signing failed"};                      //!< (INVARIANT)
inline constexpr std::string_view CLIENT_HEAD_DOMAIN{"head of another FlowMesh domain"};            //!< (VERIFY_FAILED)
inline constexpr std::string_view CLIENT_HEAD_PROFILE{"head of another profile"};                   //!< (VERIFY_FAILED)
inline constexpr std::string_view CLIENT_HEAD_COMMIT{"reply commit identity is not the header's"};  //!< (VERIFY_FAILED)
inline constexpr std::string_view CLIENT_HEAD_SCOPE{"reply scope is not the header's"};             //!< (VERIFY_FAILED)
inline constexpr std::string_view CLIENT_HEAD_LABEL{"reply is not labelled CERTIFIED"};             //!< (VERIFY_FAILED)
inline constexpr std::string_view CLIENT_NOT_FOUND{"the reply holds no account leaf"};              //!< (VERIFY_FAILED)
inline constexpr std::string_view CLIENT_WRONG_ACCOUNT{"the leaf is another account's"};            //!< (VERIFY_FAILED)
inline constexpr std::string_view CLIENT_OUTCOME_SUBJECT{"the outcome is not this action's"};       //!< (VERIFY_FAILED)
inline constexpr std::string_view CLIENT_REF_MISMATCH{"the inclusion reference is not the outcome's"}; //!< (VERIFY_FAILED)
inline constexpr std::string_view CLIENT_NOT_EXPIRED{"the reply is not a provable EXPIRED"};        //!< (VERIFY_FAILED)
inline constexpr std::string_view CLIENT_UNKNOWN_MARKET{"a fill of a market the client does not trust"}; //!< (VERIFY_FAILED)

class SimClient
{
public:
    static Result<SimClient> Create(std::string name, const ProfileParams& profile, const uint256& domain, const SeatSet& trusted,
                                    std::vector<MarketDescriptor> markets);
    //! The AccountId of client `name` (AccountId::FromKey of its TEST key), without building a client.
    static Result<AccountId> AccountOf(std::string_view name);

    const std::string& Name() const { return m_name; }
    const AccountId& Account() const { return m_account; }
    const XOnlyPubKey& Key() const { return m_xonly; }
    const uint256& Domain() const { return m_domain; }

    // ---- Outbox --------------------------------------------------------------

    //! Sign `action` with this client's key (signer set to it) and keep the exact bytes under `label`.
    Result<std::vector<unsigned char>> SignOnce(const std::string& label, ActionV2 action);
    //! A PLACE under the client's domain (or `domain`), signed once.
    Result<std::vector<unsigned char>> Place(const std::string& label, uint64_t nonce, const MarketDescriptor& market, Side side,
                                             TimeInForce tif, uint64_t price_ticks, uint64_t lots, uint64_t ref_slot,
                                             uint16_t max_delay = 16, std::optional<uint256> domain = std::nullopt);
    Result<std::vector<unsigned char>> Cancel(const std::string& label, uint64_t nonce, const OrderId& order, uint64_t ref_slot);
    Result<std::vector<unsigned char>> Transfer(const std::string& label, uint64_t nonce, const AccountId& to, const AssetId& asset,
                                                Atoms amount, uint64_t ref_slot);
    Result<std::vector<unsigned char>> Withdraw(const std::string& label, uint64_t nonce, const AssetId& asset, Atoms amount,
                                                std::vector<unsigned char> destination_script, uint64_t ref_slot);

    //! The exact stored bytes of `label`.
    Result<std::vector<unsigned char>> Bytes(std::string_view label) const;
    //! The decoded stored action of `label`.
    Result<ActionV2> Action(std::string_view label) const;
    //! A copy of `label`'s bytes with signature byte 0 flipped; never stored, never signed.
    Result<std::vector<unsigned char>> TamperedSignature(std::string_view label) const;
    size_t OutboxSize() const { return m_outbox.size(); }

    // ---- Verification (engine-off) ---------------------------------------------

    //! The certificate of the reply's head and the reply's commit, scope and label against that header.
    Result<void> VerifyHead(const read::ReplyHead& head) const;
    //! A FOUND account reply: VerifyHead, then the leaf of `account` under the head (VerifyAccountLeaf).
    Result<AccountLeafV0> VerifyAccount(const read::AccountReply& reply, const AccountId& account) const;
    //! An included outcome of action `id`: its envelope's certificate, VerifyOutcome, subject and reference.
    Result<OutcomeV2> VerifyIncluded(const read::IncludedOutcome& included, const ActionId& id) const;
    //! An EXPIRED answer for outbox `label`: VerifyExpired with the reply's head and witness.
    Result<void> VerifyExpiredReply(std::string_view label, const net::ActionStatusReply& reply) const;
    //! VerifyFill of every fill of `outcome` against the trusted markets.
    Result<void> VerifyFills(const OutcomeV2& outcome) const;

private:
    SimClient(std::string name, const ProfileParams& profile, const uint256& domain, const SeatSet& trusted,
              std::vector<MarketDescriptor> markets, CKey key);

    //! Domain, profile and certificate of one certified header.
    Result<void> VerifyEnvelope(const read::CertifiedEnvelope& envelope) const;

    std::string m_name;
    ProfileParams m_profile;
    uint256 m_profile_hash;
    uint256 m_domain;
    SeatSet m_trusted;
    std::vector<MarketDescriptor> m_markets;
    CKey m_key;
    XOnlyPubKey m_xonly;
    AccountId m_account;
    std::map<std::string, std::vector<unsigned char>, std::less<>> m_outbox;
};

} // namespace flowmesh2::sim

#endif // B3_FLOWMESH2_SIM_SIM_CLIENT_H
