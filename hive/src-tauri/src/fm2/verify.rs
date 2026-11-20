//! The client's trust inputs and its checks of what a seat answers
//! (src/flowmesh2/cli/trust.cpp and src/flowmesh2/verify/b3verify.cpp).
//!
//! The trust inputs come from the genesis document: the profile, the
//! domain, the seat set and the markets. Nothing a seat answers is believed
//! until it holds against them: a head must carry a certificate of the
//! trusted seat set over a header of the trusted domain and profile; an
//! account leaf must prove into that header's state root; an included
//! outcome must prove into its slot's outcomes root and name the action; an
//! EXPIRED answer must carry the proof that no slot can include the action.
//! Each failure names the check that failed, with the node's own wording.

use super::bytes::{put_compact, H256};
use super::crypto::{fast_aggregate_verify, BlsPublicKey, BlsSignature};
use super::genesis::Genesis;
use super::hash::{tagged, Tag};
use super::reply::{Included, ReplyHead, Witness};
use super::types::{
    classify_nonce, domain_hash, expired_at, merkle_verify, seller_fee_increment, AccountLeaf,
    Action, Certificate, Envelope, Fill, Header, Market, NonceVerdict, Outcome, Profile, MAX_SEATS,
    SOURCE_ACTION,
};
use super::Error;

/// The seats whose certificates the client accepts (core/seat_set.h).
pub struct SeatSet {
    keys: Vec<BlsPublicKey>,
    root: H256,
}

impl SeatSet {
    /// `SeatSet::FromPoP`: every key is a canonical G1 point, appears once
    /// and proves possession; the root commits to the keys in seat order.
    pub fn from_pop(registrations: &[([u8; 48], [u8; 96])]) -> Result<Self, Error> {
        if registrations.is_empty() {
            return Err(Error::Invalid("a seat set needs at least one seat".into()));
        }
        if registrations.len() > MAX_SEATS {
            return Err(Error::Oversize("more seats than the protocol allows"));
        }
        let mut message = Vec::with_capacity(3 + registrations.len() * 48);
        put_compact(&mut message, registrations.len() as u64);
        let mut keys: Vec<BlsPublicKey> = Vec::with_capacity(registrations.len());
        for (key_bytes, pop_bytes) in registrations {
            if keys.iter().any(|known| known.bytes() == key_bytes) {
                return Err(Error::Invalid("a seat key appears twice".into()));
            }
            let key = BlsPublicKey::decode(key_bytes)
                .ok_or(Error::Decode("a seat key is not a canonical G1 point"))?;
            let pop = BlsSignature::decode(pop_bytes).ok_or(Error::Decode(
                "a proof of possession is not a canonical G2 point",
            ))?;
            if !key.pop_verifies(&pop) {
                return Err(Error::Verify(
                    "a seat's proof of possession does not verify",
                ));
            }
            message.extend_from_slice(key_bytes);
            keys.push(key);
        }
        Ok(Self {
            keys,
            root: tagged(Tag::SeatSet, &[&message]),
        })
    }

    pub fn size(&self) -> usize {
        self.keys.len()
    }

    /// floor(2k/3) + 1.
    pub fn quorum(&self) -> usize {
        2 * self.keys.len() / 3 + 1
    }

    pub fn root(&self) -> &H256 {
        &self.root
    }
}

/// `SlotCertificate::SignerIndices`: the bitmap has exactly ceil(k/8) bytes
/// and no bit at or above k.
fn signer_indices(bitmap: &[u8], seats: usize) -> Option<Vec<usize>> {
    if bitmap.len() != seats.div_ceil(8) {
        return None;
    }
    let mut signers = Vec::new();
    for index in 0..bitmap.len() * 8 {
        if (bitmap[index / 8] >> (index % 8)) & 1 == 1 {
            if index >= seats {
                return None;
            }
            signers.push(index);
        }
    }
    Some(signers)
}

/// `verify::VerifyCertificate`, in the node's order of checks.
pub fn verify_certificate(
    header: &Header,
    cert: &Certificate,
    seats: &SeatSet,
) -> Result<(), Error> {
    if header.seat_set_root != seats.root {
        return Err(Error::Verify("header names another seat set"));
    }
    if cert.header_hash != header.hash() {
        return Err(Error::Verify("certificate names another header"));
    }
    let signers = signer_indices(&cert.bitmap, seats.size())
        .ok_or(Error::Verify("bitmap is not a bitmap of the seat set"))?;
    if signers.len() < seats.quorum() {
        return Err(Error::Verify("fewer signers than the quorum"));
    }
    let aggregate = BlsSignature::decode(&cert.aggregate)
        .ok_or(Error::Verify("aggregate is not a canonical G2 point"))?;
    let view_format = Error::Verify("certificate view format differs from the header version");
    let digest = match (header.commitment_version, cert.view) {
        (3, Some(view)) => header.view_bound_digest(view),
        (0 | 2, None) => header.commit_digest(),
        _ => return Err(view_format),
    };
    let keys: Vec<&BlsPublicKey> = signers.iter().map(|&index| &seats.keys[index]).collect();
    if !fast_aggregate_verify(&keys, &digest, &aggregate) {
        return Err(Error::Verify("aggregate does not verify over CommitDigest"));
    }
    Ok(())
}

/// `verify::VerifyFill`: a fill's quote and seller fee, recomputed with the
/// engine's own rules.
pub fn verify_fill(fill: &Fill, market: &Market, profile: &Profile) -> Result<(), Error> {
    if !market.valid() {
        return Err(Error::Verify(
            "market violates the profile's stable fee asset policy",
        ));
    }
    if fill.market != market.id() {
        return Err(Error::Verify("fill names another market"));
    }
    if fill.lots == 0 {
        return Err(Error::Verify("fill of zero lots"));
    }
    if market.quote_atoms(fill.lots, fill.price_ticks) != Some(fill.quote_atoms) {
        return Err(Error::Verify("quote_atoms is not lots x price x tick"));
    }
    let fee = fill
        .seller_quote_before
        .checked_add(fill.quote_atoms)
        .and_then(|after| seller_fee_increment(fill.seller_quote_before, after, profile.fee_ppm));
    if fee != Some(fill.seller_fee) {
        return Err(Error::Verify("seller_fee is not the fee increment"));
    }
    Ok(())
}

/// What the client trusts, from the genesis document.
pub struct Trust {
    pub profile: Profile,
    pub profile_hash: H256,
    pub domain: H256,
    pub seats: SeatSet,
    pub markets: Vec<(H256, Market)>,
}

impl Trust {
    /// `Trust::FromGenesis`: the regtest profile, the domain of the
    /// document's network, its seats (each must prove possession of its
    /// key) and its markets.
    pub fn from_genesis(genesis: &Genesis) -> Result<Self, Error> {
        let profile = Profile::dev_regtest();
        let registrations: Vec<([u8; 48], [u8; 96])> = genesis
            .seats
            .iter()
            .map(|seat| (seat.bls_pubkey, seat.pop))
            .collect();
        Ok(Self {
            profile_hash: profile.hash(),
            profile,
            domain: domain_hash(&genesis.modern_chain_domain, &genesis.magic),
            seats: SeatSet::from_pop(&registrations)?,
            markets: genesis
                .markets
                .iter()
                .map(|m| (m.descriptor.id(), m.descriptor.clone()))
                .collect(),
        })
    }

    /// A certified envelope of the trusted seat set, domain and profile.
    pub fn verify_envelope(&self, envelope: &Envelope) -> Result<(), Error> {
        verify_certificate(&envelope.header, &envelope.cert, &self.seats)?;
        if envelope.header.domain != self.domain {
            return Err(Error::Verify("head of another FlowMesh domain"));
        }
        if envelope.header.profile_hash != self.profile_hash {
            return Err(Error::Verify("head of another profile"));
        }
        Ok(())
    }

    /// The certificate of the reply's head, and the reply's commit, scope
    /// and label against that header.
    pub fn verify_head(&self, head: &ReplyHead) -> Result<(), Error> {
        self.verify_envelope(&head.envelope)?;
        let header = &head.envelope.header;
        if head.commit_id != header.hash()
            || head.commit_parent != header.parent_hash
            || head.commit_state_root != header.state_root
            || head.commit_sequence != header.height
        {
            return Err(Error::Verify("reply commit identity is not the header's"));
        }
        if head.scope_domain != header.domain
            || head.scope_configuration != header.profile_hash
            || head.scope_validator_set != header.seat_set_root
            || head.scope_anchor != header.anchor_hash
            || head.scope_epoch != header.authority_epoch
            || head.scope_authority_revision != u64::from(header.anchor_height)
        {
            return Err(Error::Verify("reply scope is not the header's"));
        }
        if head.label != "CERTIFIED" {
            return Err(Error::Verify("reply is not labelled CERTIFIED"));
        }
        Ok(())
    }

    /// `verify::VerifyAccountLeaf`: the leaf is under the header's state.
    fn verify_leaf(&self, header: &Header, witness: &Witness) -> Result<(), Error> {
        // The regtest profile commits with version 0: five component roots.
        if header.commitment_version != 0 || witness.roots.root() != header.state_root {
            return Err(Error::Verify("component roots do not give state_root"));
        }
        if !witness.leaf.is_canonical(&self.profile) {
            return Err(Error::Verify("account leaf is not canonical"));
        }
        if !merkle_verify(
            &witness.leaf.leaf_value(),
            &witness.proof,
            &witness.roots.accounts,
        ) {
            return Err(Error::Verify("account leaf is not under the accounts root"));
        }
        Ok(())
    }

    /// A FOUND reply's leaf of `account` under its verified head.
    pub fn verify_account<'a>(
        &self,
        head: &ReplyHead,
        witness: &'a Witness,
        account: &H256,
    ) -> Result<&'a AccountLeaf, Error> {
        self.verify_head(head)?;
        if witness.leaf.account != *account {
            return Err(Error::Verify("the leaf is another account's"));
        }
        self.verify_leaf(&head.envelope.header, witness)?;
        Ok(&witness.leaf)
    }

    /// An included outcome of action `id`: its slot's certificate, the
    /// proof, the subject, the reference and every fill.
    pub fn verify_included<'a>(
        &self,
        included: &'a Included,
        id: &H256,
    ) -> Result<&'a Outcome, Error> {
        self.verify_envelope(&included.envelope)?;
        let header = &included.envelope.header;
        let outcome = &included.outcome;
        if included.proof.size != u64::from(header.outcome_count) {
            return Err(Error::Verify("proof is not over outcome_count leaves"));
        }
        if !merkle_verify(&outcome.leaf_hash(), &included.proof, &header.outcomes_root) {
            return Err(Error::Verify("outcome is not under outcomes_root"));
        }
        let canonical = outcome
            .deltas
            .iter()
            .all(|d| d.available_delta != 0 || d.reserved_delta != 0)
            && outcome
                .deltas
                .windows(2)
                .all(|pair| (pair[0].account, pair[0].asset) < (pair[1].account, pair[1].asset));
        if !canonical {
            return Err(Error::Verify("deltas are not sorted, unique and non-zero"));
        }
        if outcome.source != SOURCE_ACTION || outcome.subject != *id {
            return Err(Error::Verify("the outcome is not this action's"));
        }
        if included.ref_height != header.height
            || included.ref_index != included.outcome_index
            || included.proof.index != included.outcome_index
        {
            return Err(Error::Verify(
                "the inclusion reference is not the outcome's",
            ));
        }
        for fill in &outcome.fills {
            let market = self
                .markets
                .iter()
                .find(|(id, _)| *id == fill.market)
                .map(|(_, market)| market)
                .ok_or(Error::Verify(
                    "a fill of a market the client does not trust",
                ))?;
            verify_fill(fill, market, &self.profile)?;
        }
        Ok(outcome)
    }

    /// `verify::VerifyExpired`: proof that `action` can never be included.
    /// The head is certified, the signer's leaf is under it, the head is past
    /// the action's window, and the leaf shows the nonce was never consumed.
    pub fn verify_expired(
        &self,
        action: &Action,
        head: &ReplyHead,
        witness: &Witness,
    ) -> Result<(), Error> {
        self.verify_head(head)?;
        let header = &head.envelope.header;
        self.verify_leaf(header, witness)?;
        if action.domain != header.domain {
            return Err(Error::Verify("action is for another domain"));
        }
        if witness.leaf.account != action.account() {
            return Err(Error::Verify("leaf is not the signer's account"));
        }
        if action.max_delay == 0 || u32::from(action.max_delay) > self.profile.max_delay_slots {
            return Err(Error::Verify("max_delay outside 1..max_delay_slots"));
        }
        if !expired_at(action.ref_slot, action.max_delay, header.height) {
            return Err(Error::Verify("height is not above ref_slot + max_delay"));
        }
        match classify_nonce(
            &witness.leaf.used_nonces,
            action.nonce,
            self.profile.nonce_window,
        ) {
            Some(NonceVerdict::Acceptable) => Ok(()),
            Some(NonceVerdict::Used) => Err(Error::Verify("nonce is in the used window")),
            Some(NonceVerdict::TooLow) => Err(Error::Verify("nonce is below a full window")),
            None => Err(Error::Verify("malformed nonce window")),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::fm2::bytes::{hex, id_from_hex, id_hex, unhex};
    use crate::fm2::reply::{parse_state, parse_status, parse_stream_event, StreamBody};
    use serde_json::Value;

    const GENESIS: &str = include_str!("../../tests/fixtures/fm2/genesis.json");
    const STATE: &str = include_str!("../../tests/fixtures/fm2/state_alice.json");
    const FILLED: &str = include_str!("../../tests/fixtures/fm2/status_filled.json");
    const RESTING: &str = include_str!("../../tests/fixtures/fm2/status_resting.json");
    const SLOTS: &str = include_str!("../../tests/fixtures/fm2/stream_slots.txt");
    const ALICE: &str = include_str!("../../tests/fixtures/fm2/alice_account.txt");
    const ALICE_ACTION: &str = include_str!("../../tests/fixtures/fm2/alice_action.hex");
    const BOB_ACTION: &str = include_str!("../../tests/fixtures/fm2/bob_action.hex");

    fn trust() -> Trust {
        Trust::from_genesis(&Genesis::parse(GENESIS).unwrap()).unwrap()
    }

    fn json(text: &str) -> Value {
        serde_json::from_str(text).unwrap()
    }

    fn action(text: &str) -> Action {
        Action::decode(&unhex(text.trim()).unwrap(), &Profile::dev_regtest()).unwrap()
    }

    /// `value` with one hex digit of the string at `path` changed.
    fn flipped(value: &Value, path: &[&str], digit: usize) -> Value {
        let mut changed = value.clone();
        let mut node = &mut changed;
        for key in path {
            node = &mut node[*key];
        }
        let mut text = node.as_str().unwrap().to_owned().into_bytes();
        text[digit] = if text[digit] == b'0' { b'1' } else { b'0' };
        *node = Value::String(String::from_utf8(text).unwrap());
        changed
    }

    #[test]
    fn the_trust_inputs_are_the_ones_fm2_cli_prints() {
        let trust = trust();
        // `fm2-cli markets` on this genesis document.
        assert_eq!(
            id_hex(&trust.domain),
            "0d73e169058a0e4b023b9a510fd67901fc062114b24ffa4761661c9cde7ed1c2"
        );
        assert_eq!(
            id_hex(trust.seats.root()),
            "3f2bf9084eec7f413a8532bc91b9729157eb76dc0f363008b6b0afc56d9a8e6e"
        );
        // The `configuration` every seat's reply head carries.
        assert_eq!(
            id_hex(&trust.profile_hash),
            "816275e199d6ca3385fcbb2e255a7e1ab7edd368adb37771f3735f4902c801c1"
        );
        assert_eq!((trust.seats.size(), trust.seats.quorum()), (4, 3));
    }

    #[test]
    fn a_seat_set_refuses_a_repeated_key_and_a_foreign_proof() {
        let genesis = Genesis::parse(GENESIS).unwrap();
        let a = (genesis.seats[0].bls_pubkey, genesis.seats[0].pop);
        let b = (genesis.seats[1].bls_pubkey, genesis.seats[1].pop);
        assert!(SeatSet::from_pop(&[a, b]).is_ok());
        assert!(matches!(SeatSet::from_pop(&[a, a]), Err(Error::Invalid(_))));
        assert_eq!(
            SeatSet::from_pop(&[(a.0, b.1)]).err(),
            Some(Error::Verify(
                "a seat's proof of possession does not verify"
            ))
        );
        assert!(SeatSet::from_pop(&[]).is_err());
        // The root commits to the order of the seats.
        assert_ne!(
            SeatSet::from_pop(&[a, b]).unwrap().root(),
            SeatSet::from_pop(&[b, a]).unwrap().root()
        );
    }

    #[test]
    fn a_real_account_reply_verifies() {
        let trust = trust();
        let reply = parse_state(&json(STATE), &trust.profile).unwrap();
        let account = id_from_hex(ALICE.trim()).unwrap();
        let leaf = trust
            .verify_account(&reply.head, reply.witness.as_ref().unwrap(), &account)
            .unwrap();
        assert_eq!(leaf.open_orders, 1);
        assert_eq!(leaf.used_nonces, [1]);
        // 1,000 B3 deposited, 10 lots of 0.01 B3 reserved, 4 of them sold.
        assert_eq!(
            (leaf.balances[0].available, leaf.balances[0].reserved),
            (99_990_000_000, 6_000_000)
        );
    }

    #[test]
    fn an_account_reply_with_any_changed_part_is_refused() {
        let trust = trust();
        let account = id_from_hex(ALICE.trim()).unwrap();
        let original = json(STATE);
        let check = |value: &Value| {
            let reply = parse_state(value, &trust.profile)?;
            trust
                .verify_account(&reply.head, reply.witness.as_ref().unwrap(), &account)
                .map(|_| ())
        };
        assert_eq!(check(&original), Ok(()));
        // The header's state root (byte 325 of the envelope).
        assert_eq!(
            check(&flipped(&original, &["head", "envelope"], 2 * 330)),
            Err(Error::Verify("certificate names another header"))
        );
        // The aggregate signature (the envelope's last 96 bytes).
        let envelope_len = original["head"]["envelope"].as_str().unwrap().len();
        let result = check(&flipped(&original, &["head", "envelope"], envelope_len - 4));
        assert!(matches!(
            result,
            Err(Error::Verify(
                "aggregate does not verify over CommitDigest"
                    | "aggregate is not a canonical G2 point"
            ))
        ));
        // The commit identity and the scope the reply repeats.
        assert_eq!(
            check(&flipped(&original, &["head", "commit", "id"], 5)),
            Err(Error::Verify("reply commit identity is not the header's"))
        );
        assert_eq!(
            check(&flipped(&original, &["head", "scope", "anchor"], 5)),
            Err(Error::Verify("reply scope is not the header's"))
        );
        let mut label = original.clone();
        label["head"]["label"] = "LOCAL".into();
        assert_eq!(
            check(&label),
            Err(Error::Verify("reply is not labelled CERTIFIED"))
        );
        // A balance in the leaf (the available amount starts at byte 65).
        assert_eq!(
            check(&flipped(&original, &["witness", "leaf", "encoded"], 2 * 66)),
            Err(Error::Verify("account leaf is not under the accounts root"))
        );
        assert_eq!(
            check(&flipped(&original, &["witness", "roots"], 70)),
            Err(Error::Verify("component roots do not give state_root"))
        );
        let proof_len = original["witness"]["proof"]["encoded"]
            .as_str()
            .unwrap()
            .len();
        assert_eq!(
            check(&flipped(
                &original,
                &["witness", "proof", "encoded"],
                proof_len - 1
            )),
            Err(Error::Verify("account leaf is not under the accounts root"))
        );
        // Another account's leaf does not answer for this one.
        let mut other = account;
        other[0] ^= 1;
        let reply = parse_state(&original, &trust.profile).unwrap();
        assert_eq!(
            trust
                .verify_account(&reply.head, reply.witness.as_ref().unwrap(), &other)
                .err(),
            Some(Error::Verify("the leaf is another account's"))
        );
    }

    #[test]
    fn a_certificate_needs_a_quorum_of_the_trusted_seats() {
        let trust = trust();
        let reply = parse_state(&json(STATE), &trust.profile).unwrap();
        let envelope = &reply.head.envelope;
        assert_eq!(trust.verify_envelope(envelope), Ok(()));
        // The same signatures claimed for fewer seats.
        let mut few = envelope.clone();
        few.cert.bitmap = vec![0b0000_0011];
        assert_eq!(
            trust.verify_envelope(&few),
            Err(Error::Verify("fewer signers than the quorum"))
        );
        // Other seats than the ones that signed.
        let mut others = envelope.clone();
        others.cert.bitmap = vec![if envelope.cert.bitmap[0] == 0b0000_0111 {
            0b0000_1110
        } else {
            0b0000_0111
        }];
        assert_eq!(
            trust.verify_envelope(&others),
            Err(Error::Verify("aggregate does not verify over CommitDigest"))
        );
        // A bit beyond the four seats, and a bitmap of the wrong length.
        let mut beyond = envelope.clone();
        beyond.cert.bitmap = vec![0b0001_0111];
        assert_eq!(
            trust.verify_envelope(&beyond),
            Err(Error::Verify("bitmap is not a bitmap of the seat set"))
        );
        let mut long = envelope.clone();
        long.cert.bitmap = vec![0b0000_1111, 0];
        assert_eq!(
            trust.verify_envelope(&long),
            Err(Error::Verify("bitmap is not a bitmap of the seat set"))
        );
        // A view on a certificate of a header that binds none.
        let mut view = envelope.clone();
        view.cert.view = Some(0);
        assert_eq!(
            trust.verify_envelope(&view),
            Err(Error::Verify(
                "certificate view format differs from the header version"
            ))
        );
    }

    #[test]
    fn a_head_of_another_seat_set_domain_or_profile_is_refused() {
        let genesis = Genesis::parse(GENESIS).unwrap();
        let reply = parse_state(&json(STATE), &Profile::dev_regtest()).unwrap();
        // A client that trusts only three of the four seats.
        let mut three = genesis.clone();
        three.seats.pop();
        let trust = Trust::from_genesis(&three).unwrap();
        assert_eq!(
            trust.verify_envelope(&reply.head.envelope),
            Err(Error::Verify("header names another seat set"))
        );
        // A client of another network (another magic gives another domain).
        let mut other = genesis.clone();
        other.magic[0] ^= 1;
        let trust = Trust::from_genesis(&other).unwrap();
        assert_eq!(
            trust.verify_envelope(&reply.head.envelope),
            Err(Error::Verify("head of another FlowMesh domain"))
        );
        // A client with other rules.
        let mut trust = Trust::from_genesis(&genesis).unwrap();
        trust.profile.fee_ppm = 200;
        trust.profile_hash = trust.profile.hash();
        assert_eq!(
            trust.verify_envelope(&reply.head.envelope),
            Err(Error::Verify("head of another profile"))
        );
    }

    #[test]
    fn a_real_fill_verifies_with_its_fee() {
        let trust = trust();
        let reply = parse_status(&json(FILLED), &trust.profile).unwrap();
        let bob = action(BOB_ACTION);
        let outcome = trust
            .verify_included(reply.included.as_ref().unwrap(), &bob.id())
            .unwrap();
        assert_eq!(outcome.code, 3); // FILLED
        assert_eq!(outcome.remaining_lots, 0);
        assert_eq!(outcome.fills.len(), 1);
        let fill = &outcome.fills[0];
        // 4 lots at 100 ticks: 400 atoms of rUSD, and the seller's first atom of fee.
        assert_eq!(
            (
                fill.lots,
                fill.price_ticks,
                fill.quote_atoms,
                fill.seller_fee
            ),
            (4, 100, 400, 1)
        );
        assert_eq!(fill.maker, action(ALICE_ACTION).id());
        assert_eq!(fill.taker, bob.id());
    }

    #[test]
    fn a_resting_order_verifies_and_is_not_another_actions_outcome() {
        let trust = trust();
        let reply = parse_status(&json(RESTING), &trust.profile).unwrap();
        let alice = action(ALICE_ACTION);
        let included = reply.included.as_ref().unwrap();
        let outcome = trust.verify_included(included, &alice.id()).unwrap();
        assert_eq!((outcome.code, outcome.remaining_lots), (2, 10)); // RESTING
        assert_eq!(
            trust
                .verify_included(included, &action(BOB_ACTION).id())
                .err(),
            Some(Error::Verify("the outcome is not this action's"))
        );
    }

    #[test]
    fn an_included_reply_with_any_changed_part_is_refused() {
        let trust = trust();
        let id = action(BOB_ACTION).id();
        let original = json(FILLED);
        let check = |value: &Value| {
            let reply = parse_status(value, &trust.profile)?;
            trust
                .verify_included(reply.included.as_ref().unwrap(), &id)
                .map(|_| ())
        };
        assert_eq!(check(&original), Ok(()));
        // The fill's lots (outcome: 83 fixed bytes, a count, then maker,
        // taker, market, side, price; lots start at byte 189).
        assert_eq!(
            check(&flipped(
                &original,
                &["included", "outcome", "encoded"],
                2 * 189 + 1
            )),
            Err(Error::Verify("outcome is not under outcomes_root"))
        );
        // The remaining lots (byte 75).
        assert_eq!(
            check(&flipped(
                &original,
                &["included", "outcome", "encoded"],
                2 * 75 + 1
            )),
            Err(Error::Verify("outcome is not under outcomes_root"))
        );
        // The outcomes root in the header (byte 293 of the envelope).
        assert_eq!(
            check(&flipped(&original, &["included", "envelope"], 2 * 300)),
            Err(Error::Verify("certificate names another header"))
        );
        // The reference the reply repeats.
        let mut height = original.clone();
        height["included"]["ref"]["height"] = 409.into();
        assert_eq!(
            check(&height),
            Err(Error::Verify(
                "the inclusion reference is not the outcome's"
            ))
        );
        let mut index = original.clone();
        index["included"]["outcome_index"] = 1.into();
        assert_eq!(
            check(&index),
            Err(Error::Verify(
                "the inclusion reference is not the outcome's"
            ))
        );
        // A proof for another position or tree size.
        let mut proof = unhex(original["included"]["proof"]["encoded"].as_str().unwrap()).unwrap();
        proof[8] = 3;
        let mut size = original.clone();
        size["included"]["proof"]["encoded"] = hex(&proof).into();
        assert_eq!(
            check(&size),
            Err(Error::Verify("proof is not over outcome_count leaves"))
        );
    }

    #[test]
    fn a_fill_is_recomputed_from_the_market() {
        let trust = trust();
        let reply = parse_status(&json(FILLED), &trust.profile).unwrap();
        let fill = reply.included.unwrap().outcome.fills[0].clone();
        let (_, market) = &trust.markets[0];
        assert_eq!(verify_fill(&fill, market, &trust.profile), Ok(()));
        let quote = Fill {
            quote_atoms: fill.quote_atoms + 1,
            ..fill.clone()
        };
        assert_eq!(
            verify_fill(&quote, market, &trust.profile),
            Err(Error::Verify("quote_atoms is not lots x price x tick"))
        );
        let fee = Fill {
            seller_fee: 0,
            ..fill.clone()
        };
        assert_eq!(
            verify_fill(&fee, market, &trust.profile),
            Err(Error::Verify("seller_fee is not the fee increment"))
        );
        let zero = Fill {
            lots: 0,
            ..fill.clone()
        };
        assert_eq!(
            verify_fill(&zero, market, &trust.profile),
            Err(Error::Verify("fill of zero lots"))
        );
        let (_, other) = &trust.markets[1];
        assert_eq!(
            verify_fill(&fill, other, &trust.profile),
            Err(Error::Verify("fill names another market"))
        );
    }

    #[test]
    fn expiry_needs_a_head_past_the_window_and_an_unused_nonce() {
        let trust = trust();
        let reply = parse_state(&json(STATE), &trust.profile).unwrap();
        let witness = reply.witness.as_ref().unwrap();
        let height = reply.head.envelope.header.height;
        // An action of alice that was never sent: nonce 2 is unused in her leaf.
        let alice = action(ALICE_ACTION);
        let unsent = |nonce: u64, ref_slot: u64, max_delay: u16| Action {
            nonce,
            ref_slot,
            max_delay,
            ..alice.clone()
        };
        assert_eq!(
            trust.verify_expired(&unsent(2, height - 10, 5), &reply.head, witness),
            Ok(())
        );
        assert_eq!(
            trust.verify_expired(&unsent(2, height - 5, 5), &reply.head, witness),
            Err(Error::Verify("height is not above ref_slot + max_delay"))
        );
        // Nonce 1 is the one her included order used.
        assert_eq!(
            trust.verify_expired(&unsent(1, height - 10, 5), &reply.head, witness),
            Err(Error::Verify("nonce is in the used window"))
        );
        assert_eq!(
            trust.verify_expired(&unsent(2, height - 10, 0), &reply.head, witness),
            Err(Error::Verify("max_delay outside 1..max_delay_slots"))
        );
        // Bob's action is not answered by alice's leaf.
        let bob = Action {
            ref_slot: height - 10,
            max_delay: 5,
            nonce: 9,
            ..action(BOB_ACTION)
        };
        assert_eq!(
            trust.verify_expired(&bob, &reply.head, witness),
            Err(Error::Verify("leaf is not the signer's account"))
        );
        let foreign = Action {
            domain: [7; 32],
            ..unsent(2, height - 10, 5)
        };
        assert_eq!(
            trust.verify_expired(&foreign, &reply.head, witness),
            Err(Error::Verify("action is for another domain"))
        );
    }

    #[test]
    fn slot_events_carry_envelopes_that_verify() {
        let trust = trust();
        let mut seen = 0;
        for line in SLOTS.lines().filter_map(|line| line.strip_prefix("data: ")) {
            let Some(event) = parse_stream_event(&json(line), &trust.profile).unwrap() else {
                continue;
            };
            let StreamBody::Slot { envelope, .. } = &event.body else {
                panic!("a slot event")
            };
            assert_eq!(trust.verify_envelope(envelope), Ok(()));
            assert_eq!(envelope.header.height, event.head_slot);
            seen += 1;
        }
        assert!(
            seen >= 2,
            "the fixture holds a snapshot and at least one delta"
        );
    }

    #[test]
    fn the_signed_actions_of_fm2_cli_verify_under_bip340() {
        for text in [ALICE_ACTION, BOB_ACTION] {
            let action = action(text);
            assert!(crate::fm2::crypto::schnorr_verify(
                &action.signer,
                &action.id(),
                &action.sig
            ));
            assert_eq!(action.domain, trust().domain);
        }
        assert_eq!(id_hex(&action(ALICE_ACTION).account()), ALICE.trim());
    }
}
