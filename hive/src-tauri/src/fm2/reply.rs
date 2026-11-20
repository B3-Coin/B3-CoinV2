//! The JSON a seat's API answers (src/flowmesh2/netio/api_json.cpp), parsed
//! into the objects the checks in verify.rs take. The API writes every
//! structure twice: as readable fields and as hex of its canonical encoding.
//! Only the encodings are read for anything that is verified, so the bytes
//! that are checked are the bytes that are shown.

use serde_json::Value;

use super::bytes::{id_from_hex, unhex, H256};
use super::types::{
    AccountLeaf, Envelope, MerkleProof, Outcome, Profile, StateRoots, SIDE_BUY, SIDE_SELL,
};
use super::Error;

const SHAPE: Error = Error::Decode("a seat's reply is not the expected JSON");

fn text<'a>(value: &'a Value, name: &str) -> Result<&'a str, Error> {
    value[name].as_str().ok_or(SHAPE)
}

fn optional_text(value: &Value, name: &str) -> String {
    value[name].as_str().unwrap_or_default().to_owned()
}

fn number(value: &Value, name: &str) -> Result<u64, Error> {
    value[name].as_u64().ok_or(SHAPE)
}

fn signed(value: &Value, name: &str) -> Result<i64, Error> {
    value[name].as_i64().ok_or(SHAPE)
}

fn id(value: &Value, name: &str) -> Result<H256, Error> {
    id_from_hex(text(value, name)?).ok_or(SHAPE)
}

fn encoded(value: &Value, name: &str) -> Result<Vec<u8>, Error> {
    unhex(text(value, name)?).ok_or(SHAPE)
}

/// The head a reply was read at: a certified header, and the commit identity
/// and scope the reply repeats (claims until `Trust::verify_head`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ReplyHead {
    pub scope_domain: H256,
    pub scope_configuration: H256,
    pub scope_validator_set: H256,
    pub scope_anchor: H256,
    pub scope_epoch: u64,
    pub scope_authority_revision: u64,
    pub commit_id: H256,
    pub commit_parent: H256,
    pub commit_state_root: H256,
    pub commit_sequence: u64,
    pub envelope: Envelope,
    pub label: String,
}

fn parse_head(value: &Value) -> Result<ReplyHead, Error> {
    let scope = &value["scope"];
    let commit = &value["commit"];
    Ok(ReplyHead {
        scope_domain: id(scope, "domain")?,
        scope_configuration: id(scope, "configuration")?,
        scope_validator_set: id(scope, "validator_set")?,
        scope_anchor: id(scope, "anchor")?,
        scope_epoch: number(scope, "epoch")?,
        scope_authority_revision: number(scope, "authority_revision")?,
        commit_id: id(commit, "id")?,
        commit_parent: id(commit, "parent")?,
        commit_state_root: id(commit, "state_root")?,
        commit_sequence: number(commit, "sequence")?,
        envelope: Envelope::decode(&encoded(value, "envelope")?)?,
        label: text(value, "label")?.to_owned(),
    })
}

/// An account leaf with the component roots and its Merkle proof.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Witness {
    pub leaf: AccountLeaf,
    pub roots: StateRoots,
    pub proof: MerkleProof,
}

fn parse_witness(value: &Value, profile: &Profile) -> Result<Witness, Error> {
    Ok(Witness {
        leaf: AccountLeaf::decode(&encoded(&value["leaf"], "encoded")?, profile)?,
        roots: StateRoots::decode(&encoded(value, "roots")?)?,
        proof: MerkleProof::decode(&encoded(&value["proof"], "encoded")?)?,
    })
}

/// An outcome with its slot's certified header and its Merkle proof.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Included {
    pub ref_height: u64,
    pub ref_index: u64,
    pub envelope: Envelope,
    pub outcome_index: u64,
    pub outcome: Outcome,
    pub proof: MerkleProof,
}

fn parse_included(value: &Value, profile: &Profile) -> Result<Included, Error> {
    Ok(Included {
        ref_height: number(&value["ref"], "height")?,
        ref_index: number(&value["ref"], "index")?,
        envelope: Envelope::decode(&encoded(value, "envelope")?)?,
        outcome_index: number(value, "outcome_index")?,
        outcome: Outcome::decode(&encoded(&value["outcome"], "encoded")?, profile)?,
        proof: MerkleProof::decode(&encoded(&value["proof"], "encoded")?)?,
    })
}

/// `GET /v2/state/<account>`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StateReply {
    pub head: ReplyHead,
    /// False is a seat's claim: commitment version 0 has no absence proof.
    pub found: bool,
    pub reason: String,
    pub witness: Option<Witness>,
}

pub fn parse_state(value: &Value, profile: &Profile) -> Result<StateReply, Error> {
    let found = match text(value, "status")? {
        "FOUND" => true,
        "UNKNOWN" => false,
        _ => return Err(SHAPE),
    };
    let witness = match value.get("witness") {
        Some(witness) => Some(parse_witness(witness, profile)?),
        None => None,
    };
    if found != witness.is_some() {
        return Err(SHAPE);
    }
    Ok(StateReply {
        head: parse_head(&value["head"])?,
        found,
        reason: optional_text(value, "reason"),
        witness,
    })
}

/// `POST /v2/actions/status`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StatusReply {
    /// INCLUDED, PENDING, EXPIRED or UNKNOWN.
    pub status: String,
    pub reason: String,
    pub head: Option<ReplyHead>,
    pub included: Option<Included>,
    pub expiry: Option<Witness>,
    /// What the seat's pool holds, when PENDING: its status name.
    pub pending: Option<String>,
}

/// The reason of an UNKNOWN answer whose pool dropped the action for a nonce
/// reason without a proof (net/client_api.h).
pub const UNKNOWN_TERMINAL_UNPROVEN: &str =
    "this node's pool dropped it, but the lease proves nothing";

pub fn parse_status(value: &Value, profile: &Profile) -> Result<StatusReply, Error> {
    let status = text(value, "status")?;
    if !matches!(status, "INCLUDED" | "PENDING" | "EXPIRED" | "UNKNOWN") {
        return Err(SHAPE);
    }
    let head = value.get("head").map(parse_head).transpose()?;
    let included = value
        .get("included")
        .map(|v| parse_included(v, profile))
        .transpose()?;
    let expiry = value
        .get("expiry")
        .map(|v| parse_witness(v, profile))
        .transpose()?;
    let pending = value
        .get("pending")
        .map(|v| text(v, "status").map(str::to_owned))
        .transpose()?;
    Ok(StatusReply {
        status: status.to_owned(),
        reason: optional_text(value, "reason"),
        head,
        included,
        expiry,
        pending,
    })
}

/// `POST /v2/actions`: what one seat's admission said. Never certified.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AdmissionReply {
    pub code: String,
    pub reason: String,
    pub head_height: Option<u64>,
}

pub fn parse_admission(value: &Value) -> Result<AdmissionReply, Error> {
    Ok(AdmissionReply {
        code: text(value, "code")?.to_owned(),
        reason: optional_text(value, "reason"),
        head_height: value["head_height"].as_u64(),
    })
}

/// A seat's refusal: `{"error": code, "reason": text}`.
pub fn parse_error(value: &Value) -> Option<(String, String)> {
    Some((
        value["error"].as_str()?.to_owned(),
        optional_text(value, "reason"),
    ))
}

/// `GET /v2/health`: a seat's own diagnostics, never certified.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Health {
    pub fenced: bool,
    pub published_height: Option<u64>,
    pub pool_height: u64,
    pub pool_live: u64,
}

pub fn parse_health(value: &Value) -> Result<Health, Error> {
    Ok(Health {
        fenced: value["fenced"].as_bool().ok_or(SHAPE)?,
        published_height: value["published_height"].as_u64(),
        pool_height: number(value, "pool_height")?,
        pool_live: number(value, "pool_live")?,
    })
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct BookLevel {
    pub price_ticks: u64,
    pub lots: u64,
    pub orders: u64,
}

/// A fill as the trades stream lists it. It lacks the seller's cumulative
/// quote, so its fee cannot be recomputed: a seat's claim.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StreamFill {
    pub maker: H256,
    pub taker: H256,
    pub market: H256,
    pub taker_side: u8,
    pub price_ticks: u64,
    pub lots: u64,
    pub quote_atoms: i64,
    pub seller_fee: i64,
}

// Short-lived: one per stream event.
#[allow(clippy::large_enum_variant)]
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum StreamBody {
    /// A committed slot's header and certificate: verifiable.
    Slot {
        envelope: Envelope,
        action_count: u64,
        outcome_count: u64,
    },
    /// One account's leaf after a slot, without a proof: a seat's claim.
    Account {
        account: H256,
        leaf: Option<AccountLeaf>,
    },
    /// One market's price ladder: a seat's claim.
    Book {
        market: H256,
        bids: Vec<BookLevel>,
        asks: Vec<BookLevel>,
    },
    /// One market's fills of a slot: a seat's claim.
    Trades {
        market: H256,
        fills: Vec<StreamFill>,
    },
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StreamEvent {
    /// `epoch:seq:head`, what a resubscription names with `from=`.
    pub cursor: String,
    pub snapshot: bool,
    pub head_slot: u64,
    pub body: StreamBody,
}

fn parse_levels(value: &Value) -> Result<Vec<BookLevel>, Error> {
    value
        .as_array()
        .ok_or(SHAPE)?
        .iter()
        .map(|level| {
            Ok(BookLevel {
                price_ticks: number(level, "price_ticks")?,
                lots: number(level, "lots")?,
                orders: number(level, "orders")?,
            })
        })
        .collect()
}

/// The `data:` object of one stream event; `None` for a heartbeat (`{}`).
pub fn parse_stream_event(value: &Value, profile: &Profile) -> Result<Option<StreamEvent>, Error> {
    if value.as_object().is_some_and(|object| object.is_empty()) {
        return Ok(None);
    }
    let snapshot = match text(value, "kind")? {
        "snapshot" => true,
        "delta" => false,
        _ => return Err(SHAPE),
    };
    let body = match text(value, "channel")? {
        "slots" => StreamBody::Slot {
            envelope: Envelope::decode(&encoded(value, "envelope")?)?,
            action_count: number(value, "action_count")?,
            outcome_count: number(value, "outcome_count")?,
        },
        "account" => StreamBody::Account {
            account: id(value, "account")?,
            leaf: match value.get("leaf") {
                Some(leaf) => Some(AccountLeaf::decode(&encoded(leaf, "encoded")?, profile)?),
                None => None,
            },
        },
        "book" => StreamBody::Book {
            market: id(value, "market")?,
            bids: parse_levels(&value["bids"])?,
            asks: parse_levels(&value["asks"])?,
        },
        "trades" => StreamBody::Trades {
            market: id(value, "market")?,
            fills: value["fills"]
                .as_array()
                .ok_or(SHAPE)?
                .iter()
                .map(|fill| {
                    Ok(StreamFill {
                        maker: id(fill, "maker")?,
                        taker: id(fill, "taker")?,
                        market: id(fill, "market")?,
                        taker_side: match text(fill, "taker_side")? {
                            "buy" => SIDE_BUY,
                            "sell" => SIDE_SELL,
                            _ => return Err(SHAPE),
                        },
                        price_ticks: number(fill, "price_ticks")?,
                        lots: number(fill, "lots")?,
                        quote_atoms: signed(fill, "quote_atoms")?,
                        seller_fee: signed(fill, "seller_fee")?,
                    })
                })
                .collect::<Result<_, Error>>()?,
        },
        _ => return Err(SHAPE),
    };
    Ok(Some(StreamEvent {
        cursor: text(value, "cursor")?.to_owned(),
        snapshot,
        head_slot: number(value, "head_slot")?,
        body,
    }))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn json(text: &str) -> Value {
        serde_json::from_str(text).unwrap()
    }

    fn events(text: &str) -> Vec<StreamEvent> {
        let profile = Profile::dev_regtest();
        text.lines()
            .filter_map(|line| line.strip_prefix("data: "))
            .filter_map(|line| parse_stream_event(&json(line), &profile).unwrap())
            .collect()
    }

    #[test]
    fn an_unknown_account_is_a_claim_without_a_witness() {
        let reply = parse_state(
            &json(include_str!("../../tests/fixtures/fm2/state_unknown.json")),
            &Profile::dev_regtest(),
        )
        .unwrap();
        assert!(!reply.found && reply.witness.is_none());
        assert!(!reply.reason.is_empty());
    }

    #[test]
    fn a_found_reply_without_its_witness_is_malformed() {
        let mut value = json(include_str!("../../tests/fixtures/fm2/state_alice.json"));
        value.as_object_mut().unwrap().remove("witness");
        assert_eq!(parse_state(&value, &Profile::dev_regtest()), Err(SHAPE));
    }

    #[test]
    fn health_and_admission_replies_parse() {
        let health =
            parse_health(&json(include_str!("../../tests/fixtures/fm2/health.json"))).unwrap();
        assert!(!health.fenced && health.published_height.is_some());
        let admitted = json(
            r#"{"code":"ADMITTED","reason":"admitted to this node's pool","head_height":407}"#,
        );
        let reply = parse_admission(&admitted).unwrap();
        assert_eq!(
            (reply.code.as_str(), reply.head_height),
            ("ADMITTED", Some(407))
        );
        let refused = json(r#"{"error":"BUSY","reason":"request rate limit"}"#);
        assert_eq!(
            parse_error(&refused),
            Some(("BUSY".to_owned(), "request rate limit".to_owned()))
        );
        assert_eq!(parse_error(&admitted), None);
    }

    #[test]
    fn the_book_stream_gives_the_price_ladder() {
        let events = events(include_str!("../../tests/fixtures/fm2/stream_book.txt"));
        assert_eq!(events.len(), 1);
        assert!(events[0].snapshot);
        let StreamBody::Book { bids, asks, .. } = &events[0].body else {
            panic!("a book event")
        };
        assert!(bids.is_empty());
        assert_eq!(
            asks,
            &[BookLevel {
                price_ticks: 100,
                lots: 6,
                orders: 1
            }]
        );
    }

    #[test]
    fn the_trades_stream_gives_a_snapshot_then_fills() {
        let events = events(include_str!("../../tests/fixtures/fm2/stream_trades.txt"));
        assert_eq!(events.len(), 2);
        let StreamBody::Trades { fills, .. } = &events[0].body else {
            panic!("a trades event")
        };
        assert!(events[0].snapshot && fills.is_empty());
        let StreamBody::Trades { fills, .. } = &events[1].body else {
            panic!("a trades event")
        };
        assert_eq!(
            (
                fills[0].taker_side,
                fills[0].price_ticks,
                fills[0].lots,
                fills[0].quote_atoms
            ),
            (SIDE_BUY, 100, 1, 100)
        );
        assert_eq!(
            events[1].cursor.rsplit(':').next(),
            Some(events[1].head_slot.to_string().as_str())
        );
    }

    #[test]
    fn the_account_stream_gives_leaves() {
        let events = events(include_str!("../../tests/fixtures/fm2/stream_account.txt"));
        assert!(events.len() >= 2);
        let StreamBody::Account {
            leaf: Some(first), ..
        } = &events[0].body
        else {
            panic!("an account event")
        };
        let StreamBody::Account {
            leaf: Some(second), ..
        } = &events[1].body
        else {
            panic!("an account event")
        };
        // The sale of 4 lots between the two events released 4,000,000 reserved atoms of B3.
        assert_eq!(
            first.balances[0].reserved - second.balances[0].reserved,
            4_000_000
        );
    }

    #[test]
    fn a_heartbeat_is_no_event_and_garbage_is_refused() {
        let profile = Profile::dev_regtest();
        assert_eq!(parse_stream_event(&json("{}"), &profile), Ok(None));
        assert_eq!(
            parse_stream_event(&json(r#"{"kind":"delta","channel":"nope"}"#), &profile),
            Err(SHAPE)
        );
        assert_eq!(
            parse_stream_event(&json(r#"{"kind":"other","channel":"book"}"#), &profile),
            Err(SHAPE)
        );
    }
}
