// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_VERIFY_B3VERIFY_H
#define B3_FLOWMESH2_VERIFY_B3VERIFY_H

#include <flowmesh2/core/account_leaf.h>
#include <flowmesh2/core/action.h>
#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/authority.h>
#include <flowmesh2/core/certificate.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/market.h>
#include <flowmesh2/core/merkle.h>
#include <flowmesh2/core/outcome.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/seat_set.h>
#include <flowmesh2/core/slot.h>
#include <flowmesh2/core/state_root.h>
#include <uint256.h>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

/** b3verify: the engine-off client verifier (slice R11; ARCHITECTURE.md
 * sections 3 and 10.2; plan section 3.12; SEC-5).
 *
 * A client trusts three inputs it obtained out of band: the seat set
 * (SeatSet, PoP-verified keys), the profile (ProfileParams; the header's
 * profile_hash must be ProfileHash of it) and the market descriptors. It
 * trusts nothing the read API says: every claim a reply makes is checked
 * here from the certified header, the certificate and Merkle proofs. In the
 * outline the trusted seat set is the k = 1 REGTEST dev seat (O-10); the
 * label "certified" means exactly "verified against that seat set" and is
 * never merged with "checkpointed" or "B3-final" (INV-13).
 *
 * Library boundary: fm2_verify links fm2_types and bitcoin_crypto only. It
 * includes core/ headers only, never ledger/, engine/, state/, storage/ or
 * read/ (guard rules 1, 3 and 4), so it re-derives every value it checks
 * from the same core spec functions the engine uses (core/fee_math.h,
 * core/market.h, core/nonce_rule.h, core/merkle.h, core/state_root.h).
 *
 * Every function is pure and deterministic (no clock, I/O, lock or
 * randomness). Success is Ok(); a claim that does not hold is VERIFY_FAILED
 * with one of the static details below, so a test or a UI can tell which
 * check failed. A profile that Validate() refuses returns Validate's error
 * (including its typed stubs). The three stubs at the end return
 * NotImplemented with their task ID and never a partial success.
 */
namespace flowmesh2::verify {

// Static details, all VERIFY_FAILED.
inline constexpr std::string_view CERT_SEAT_SET{"header names another seat set"};
inline constexpr std::string_view CERT_HEADER{"certificate names another header"};
inline constexpr std::string_view CERT_BITMAP{"bitmap is not a bitmap of the seat set"};
inline constexpr std::string_view CERT_QUORUM{"fewer signers than the quorum"};
inline constexpr std::string_view CERT_AGGREGATE{"aggregate is not a canonical G2 point"};
inline constexpr std::string_view CERT_SIGNATURE{"aggregate does not verify over CommitDigest"};
inline constexpr std::string_view OUTCOME_PROOF_SIZE{"proof is not over outcome_count leaves"};
inline constexpr std::string_view OUTCOME_PROOF{"outcome is not under outcomes_root"};
inline constexpr std::string_view OUTCOME_DELTAS{"deltas are not sorted, unique and non-zero"};
inline constexpr std::string_view PROFILE_MISMATCH{"profile is not the header's profile"};
inline constexpr std::string_view LEAF_STATE_ROOTS{"component roots do not give state_root"};
inline constexpr std::string_view LEAF_NOT_CANONICAL{"account leaf is not canonical"};
inline constexpr std::string_view LEAF_PROOF{"account leaf is not under the accounts root"};
inline constexpr std::string_view FILL_MARKET{"fill names another market"};
inline constexpr std::string_view FILL_LOTS{"fill of zero lots"};
inline constexpr std::string_view FILL_QUOTE{"quote_atoms is not lots x price x tick"};
inline constexpr std::string_view FILL_FEE{"seller_fee is not the fee increment"};
inline constexpr std::string_view TRANSITION_ACCOUNT{"leaves of two accounts"};
inline constexpr std::string_view TRANSITION_OVERFLOW{"deltas overflow"};
inline constexpr std::string_view TRANSITION_BALANCE{"after is not before plus the deltas"};
inline constexpr std::string_view EXPIRED_DOMAIN{"action is for another domain"};
inline constexpr std::string_view EXPIRED_ACCOUNT{"leaf is not the signer's account"};
inline constexpr std::string_view EXPIRED_MAX_DELAY{"max_delay outside 1..max_delay_slots"};
inline constexpr std::string_view EXPIRED_NOT_YET{"height is not above ref_slot + max_delay"};
inline constexpr std::string_view EXPIRED_NONCE_USED{"nonce is in the used window"};
inline constexpr std::string_view EXPIRED_NONCE_BELOW{"nonce is below a full window"};

/** The certificate check. In this order:
 * 1. header.seat_set_root == seats.Root()            (CERT_SEAT_SET)
 * 2. cert.header_hash == header.Hash()                (CERT_HEADER)
 * 3. the bitmap is exactly ceil(k/8) bytes with no bit at or above k
 *    (SlotCertificate::SignerIndices)                 (CERT_BITMAP)
 * 4. at least seats.Quorum() = floor(2k/3) + 1 signers (CERT_QUORUM)
 * 5. the aggregate decodes as a canonical G2 point    (CERT_AGGREGATE)
 * 6. bls::FastAggregateVerify of the marked seats' PoP-verified keys over
 *    header.CommitDigest()                            (CERT_SIGNATURE)
 * A certificate is not unique (O-4b): any quorum subset passes, so two
 * valid certificates of one header may differ.
 */
Result<void> VerifyCertificate(const SlotHeaderV2& header, const SlotCertificate& cert, const SeatSet& seats);

/** `outcome` is a certified outcome of `header`: proof.size equals
 * header.outcome_count (OUTCOME_PROOF_SIZE), MerkleVerify(outcome.LeafHash(),
 * proof, header.outcomes_root) (OUTCOME_PROOF), and the deltas are the
 * engine's output form (OUTCOME_DELTAS): strictly ascending by (account,
 * asset), no entry with both deltas zero. The header itself is checked by
 * VerifyCertificate.
 */
Result<void> VerifyOutcome(const SlotHeaderV2& header, const OutcomeV2& outcome, const MerkleProof& proof);

/** `leaf` is the v0 account leaf of its account in the state `header`
 * commits to: Validate(profile) and ProfileHash(profile) ==
 * header.profile_hash (PROFILE_MISMATCH); roots.Root() == header.state_root
 * (LEAF_STATE_ROOTS); the leaf is canonical under the profile
 * (LEAF_NOT_CANONICAL); MerkleVerify(leaf.LeafValue(), proof,
 * roots.accounts) (LEAF_PROOF). Commitment v0 only (the header codec
 * refuses another commitment_version); v0 has no absence proof, so an
 * absent account cannot be proved (M2-LED-02).
 */
Result<void> VerifyAccountLeaf(const SlotHeaderV2& header, const AccountLeafV0& leaf, const StateRootsV0& roots,
                               const MerkleProof& proof, const ProfileParams& profile);

/** A fill's quote and seller fee, recomputed with the engine's own spec
 * functions: Validate(profile) (its errors, e.g. the PER_SIDE stub);
 * fill.market == market.Id() (FILL_MARKET); lots > 0 (FILL_LOTS);
 * QuoteAtoms(market, lots, price_ticks) == quote_atoms (FILL_QUOTE; an
 * overflow fails the same way); SellerFeeIncrement(seller_quote_before,
 * seller_quote_before + quote_atoms, fee_ppm) == seller_fee (FILL_FEE).
 */
Result<void> VerifyFill(const Fill& fill, const MarketDescriptor& market, const ProfileParams& profile);

/** `after` is `before` changed by exactly the deltas of `outcomes` that
 * name the account: before.account == after.account (TRANSITION_ACCOUNT);
 * for every asset of either leaf or of a delta, before + sum(deltas) ==
 * after for both available and reserved (TRANSITION_BALANCE), with checked
 * arithmetic (TRANSITION_OVERFLOW). The caller verifies the two leaves
 * (VerifyAccountLeaf under consecutive headers) and every outcome
 * (VerifyOutcome under the later header). Only balances are compared;
 * nonces and open orders are not.
 */
Result<void> VerifyBalanceTransition(const AccountLeafV0& before, const AccountLeafV0& after,
                                     std::span<const OutcomeV2> outcomes);

/** Proof that `action` can never be included (ARCHITECTURE.md 10.2, N3):
 * 1. VerifyCertificate(header, cert, seats);
 * 2. VerifyAccountLeaf(header, leaf, roots, proof, profile);
 * 3. action.domain == header.domain                  (EXPIRED_DOMAIN)
 * 4. leaf.account == action.Account()                 (EXPIRED_ACCOUNT)
 * 5. action.MaxDelayValid(profile)                    (EXPIRED_MAX_DELAY)
 * 6. ExpiredAt(ref_slot, max_delay, header.height), i.e. height > ref_slot
 *    + max_delay (EXPIRED_NOT_YET): no slot at or after header.height + 1
 *    may include it;
 * 7. the core nonce rule says the nonce is acceptable in the leaf's used
 *    window: USED (EXPIRED_NONCE_USED), TOO_LOW in a full window
 *    (EXPIRED_NONCE_BELOW). By the lemma of core/nonce_rule.h an
 *    acceptable nonce was never consumed, and every executed action
 *    consumes its nonce, so no slot up to header.height included it.
 * The height rule is the slice's (strictly above ref_slot + max_delay).
 * height == ref_slot + max_delay would already suffice, one slot earlier
 * (O-63).
 */
Result<void> VerifyExpired(const ActionV2& action, const SlotHeaderV2& header, const SlotCertificate& cert,
                           const SeatSet& seats, const AccountLeafV0& leaf, const StateRootsV0& roots,
                           const MerkleProof& proof, const ProfileParams& profile);

// ---- Stubs ---------------------------------------------------------------------

/** Target shape of a liability-sum path (the per-asset Merkle-sum tree is
 * M2-LED-03): the leaf's sum for `asset` and its path of (sibling hash,
 * sibling sum). M5-VFY-03 may reshape it.
 */
struct LiabilityPath {
    AssetId asset;
    Atoms leaf_sum{0};
    uint64_t index{0};
    uint64_t size{0};
    std::vector<uint256> sibling_hashes;
    std::vector<Atoms> sibling_sums;
};

/** Every sum on the path is in [0, 2^62] and the path reaches the header's
 * liability root for the asset (plan section 3.12: clients check their own
 * leaf at every accepted checkpoint). Stub: NotImplemented(M5-VFY-03).
 */
Result<void> VerifyLiabilityPath(const SlotHeaderV2& header, const AccountLeafV0& leaf, const LiabilityPath& path);

/** Target shape of a checkpoint claim (M8-CKP-01 defines the checkpoint and
 * its builder): the certified slot it commits and its withdrawal
 * message_root.
 */
struct CheckpointClaim {
    uint64_t height{0};
    uint256 header_hash;
    uint256 state_root;
    uint256 message_root;
};

/** A checkpoint matches a certified header and the seats signed it. Stub:
 * NotImplemented(M8-CKP-01).
 */
Result<void> VerifyCheckpoint(const CheckpointClaim& checkpoint, const SlotHeaderV2& header, const SlotCertificate& cert,
                              const SeatSet& seats);

/** Target shape of an anchor proof (M7-ADP-02): the B3 block the slot is
 * anchored to and the evidence that it is on the finalized chain (header
 * chain bytes and a fact inclusion path).
 */
struct AnchorProof {
    Anchor anchor;
    std::vector<unsigned char> chain_evidence;
    MerkleProof inclusion;
};

/** The header's anchor is a finalized B3 block and its facts were taken
 * from it. Stub: NotImplemented(M7-ADP-02).
 */
Result<void> VerifyAnchorProof(const SlotHeaderV2& header, const AnchorProof& proof);

} // namespace flowmesh2::verify

#endif // B3_FLOWMESH2_VERIFY_B3VERIFY_H
