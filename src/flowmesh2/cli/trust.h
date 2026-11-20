// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CLI_TRUST_H
#define B3_FLOWMESH2_CLI_TRUST_H

#include <flowmesh2/core/account_leaf.h>
#include <flowmesh2/core/action.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/market.h>
#include <flowmesh2/core/outcome.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/seat_set.h>
#include <flowmesh2/netio/genesis_doc.h>
#include <flowmesh2/read/read_service.h>
#include <flowmesh2/read/views.h>
#include <uint256.h>

#include <span>
#include <string_view>
#include <vector>

/** The client's trust inputs and its engine-off verification (M5-CLI-01,
 * ARCHITECTURE.md O-106; 14.4 P2-P5 for the simulated client this follows). The
 * trust inputs come from genesis.json (netio/genesis_doc.h): the profile,
 * the domain, the seat set and the markets. Nothing a seat answers is
 * believed until it verifies against them with verify/b3verify: a reply head
 * must carry a certificate of the trusted seat set over a header of the
 * trusted domain and profile whose identity the reply's commit and scope
 * repeat; a leaf must prove into that header's state root; an included
 * outcome must prove into its slot's outcomes root and name the action; an
 * EXPIRED answer must carry the proof VerifyExpired accepts. Every failure is
 * VERIFY_FAILED (or b3verify's own detail) and the reply is dropped.
 */
namespace flowmesh2::cli {

inline constexpr std::string_view TRUST_HEAD_DOMAIN{"head of another FlowMesh domain"};               //!< (VERIFY_FAILED)
inline constexpr std::string_view TRUST_HEAD_PROFILE{"head of another profile"};                      //!< (VERIFY_FAILED)
inline constexpr std::string_view TRUST_HEAD_COMMIT{"reply commit identity is not the header's"};     //!< (VERIFY_FAILED)
inline constexpr std::string_view TRUST_HEAD_SCOPE{"reply scope is not the header's"};                //!< (VERIFY_FAILED)
inline constexpr std::string_view TRUST_HEAD_LABEL{"reply is not labelled CERTIFIED"};                //!< (VERIFY_FAILED)
inline constexpr std::string_view TRUST_WRONG_ACCOUNT{"the leaf is another account's"};               //!< (VERIFY_FAILED)
inline constexpr std::string_view TRUST_OUTCOME_SUBJECT{"the outcome is not this action's"};          //!< (VERIFY_FAILED)
inline constexpr std::string_view TRUST_REF_MISMATCH{"the inclusion reference is not the outcome's"}; //!< (VERIFY_FAILED)
inline constexpr std::string_view TRUST_UNKNOWN_MARKET{"a fill of a market the client does not trust"}; //!< (VERIFY_FAILED)

class Trust
{
public:
    //! From the genesis document (REGTEST: DevRegtestProfile).
    static Result<Trust> FromGenesis(const netio::GenesisDoc& genesis);
    //! From the parts (the tests' simulated node).
    Trust(ProfileParams profile, uint256 domain, SeatSet seats, std::vector<MarketDescriptor> markets);

    const ProfileParams& Profile() const { return m_profile; }
    const uint256& Domain() const { return m_domain; }
    const SeatSet& Seats() const { return m_seats; }
    std::span<const MarketDescriptor> Markets() const { return m_markets; }

    //! A certified envelope of the trusted seat set, domain and profile.
    Result<void> VerifyEnvelope(const read::CertifiedEnvelope& envelope) const;
    //! The certificate of the reply's head and the reply's commit, scope and label against that header.
    Result<void> VerifyHead(const read::ReplyHead& head) const;
    //! A FOUND reply's leaf of `account` under its verified head.
    Result<AccountLeafV0> VerifyAccount(const read::ReplyHead& head, const read::AccountWitness& witness, const AccountId& account) const;
    //! An included outcome of action `id`: its envelope's certificate, the proof, the subject and the reference; its fills too.
    Result<OutcomeV2> VerifyIncluded(const read::IncludedOutcome& included, const ActionId& id) const;
    //! An EXPIRED answer for `action` under `head` with the signer's leaf witness.
    Result<void> VerifyExpired(const ActionV2& action, const read::ReplyHead& head, const read::AccountWitness& witness) const;
    //! VerifyFill of every fill against the trusted markets.
    Result<void> VerifyFills(const OutcomeV2& outcome) const;

private:
    ProfileParams m_profile;
    uint256 m_profile_hash;
    uint256 m_domain;
    SeatSet m_seats;
    std::vector<MarketDescriptor> m_markets;
};

} // namespace flowmesh2::cli

#endif // B3_FLOWMESH2_CLI_TRUST_H
