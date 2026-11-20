//! One connection to a FlowMesh V2 network: the trust inputs, the seats, the
//! account's key and its outbox, and the client's rules
//! (src/flowmesh2/cli/fm2_cli_main.cpp is the node's own client):
//!
//! - an action is signed once, is durable in the outbox before it leaves,
//!   and is posted to EVERY seat;
//! - its nonce is one above the highest of the account's verified leaf and
//!   of everything this outbox ever signed;
//! - its `ref_slot` is a little below the highest verified head and never
//!   below the lowest, so neither a lagging seat nor a fast one refuses it;
//! - nothing a seat answers about state or about an action's end is used
//!   before it verified against the genesis document (verify.rs). What
//!   cannot be verified (an unknown account, a pool's status, the book and
//!   trade streams) is passed on as that seat's claim and named as such.

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, OnceLock};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use serde_json::Value;

use super::bytes::{hex, id_hex, H256};
use super::crypto::SecretKey;
use super::genesis::{parse_pins, Genesis};
use super::hash::sha256;
use super::http::{request, stream, SeatEndpoint, StreamEnd};
use super::outbox::{create_key, load_key, Certified, Entry, Outbox, SeenFill};
use super::reply::{
    parse_admission, parse_error, parse_health, parse_state, parse_status, parse_stream_event,
    Health, StreamBody, StreamEvent, UNKNOWN_TERMINAL_UNPROVEN,
};
use super::types::{account_of, AccountLeaf, Action, Outcome, Payload};
use super::verify::Trust;
use super::Error;

/// `ref_slot` is this many slots below the highest verified head.
const REF_SLOT_LAG: u64 = 4;
/// How long an action stays includable when the caller names no window.
pub const DEFAULT_MAX_DELAY: u16 = 64;
/// A seat heartbeats every second; a stream silent this long is dead.
const STREAM_IDLE: Duration = Duration::from_secs(6);
const STREAM_RETRY: Duration = Duration::from_millis(1500);

/// What one seat answered about an account.
#[derive(Clone, Debug)]
pub struct SeatState {
    pub seat: u32,
    /// The seat's head, once its certificate verified.
    pub height: Option<u64>,
    /// The verified leaf. `None` with a height means the seat says the
    /// account does not exist, which nothing proves.
    pub leaf: Option<AccountLeaf>,
    /// Why there is no verified answer.
    pub error: Option<String>,
}

/// One seat's admission answer to a posted action. Never certified.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SeatAdmission {
    pub seat: u32,
    /// ADMITTED, DUPLICATE, a refusal code, or empty when the seat gave none.
    pub code: String,
    pub reason: String,
}

impl SeatAdmission {
    /// The seat holds the action: in its pool now, or already included.
    pub fn accepted(&self) -> bool {
        matches!(self.code.as_str(), "ADMITTED" | "DUPLICATE")
    }
}

#[derive(Clone, Debug)]
pub struct Submitted {
    pub label: String,
    pub action_id: H256,
    pub nonce: u64,
    pub ref_slot: u64,
    pub max_delay: u16,
    pub admissions: Vec<SeatAdmission>,
}

/// Where an outbox action stands after asking every seat once.
// Short-lived: one per status poll.
#[allow(clippy::large_enum_variant)]
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Progress {
    /// Certified and recorded in the outbox. `seat` is the seat whose proof
    /// was checked now; `None` when the end was recorded earlier.
    Included {
        seat: Option<u32>,
        height: u64,
        index: u32,
        outcome: Outcome,
    },
    /// Certified and recorded: no slot can include it any more.
    Expired { seat: Option<u32>, height: u64 },
    /// EVERY seat of the network answered that its pool dropped the action
    /// for a nonce reason. Nothing certifies that, so it is not recorded.
    Failed,
    /// No certified end yet. `pending` seats hold it in their pools;
    /// `refused_claims` counts INCLUDED or EXPIRED answers whose proof did
    /// not verify (never acted on).
    Waiting {
        pending: Vec<u32>,
        refused_claims: u32,
        detail: String,
    },
}

/// What a stream loop reports.
// Short-lived: one per stream event.
#[allow(clippy::large_enum_variant)]
#[derive(Clone, Debug)]
pub enum StreamUpdate {
    /// The stream is live on `seat`, or broke for the reason given.
    Link {
        seat: u32,
        live: bool,
        detail: String,
    },
    /// One event from `seat`. `verified` is true only for a slot event whose
    /// certificate held; every other body is that seat's claim.
    Event {
        seat: u32,
        verified: bool,
        event: StreamEvent,
    },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Channel {
    Slots,
    Account(H256),
    Book(H256),
    Trades(H256),
}

impl Channel {
    fn path(&self, from: Option<&str>) -> String {
        let mut path = match self {
            Channel::Slots => "/v2/stream?channel=slots".to_owned(),
            Channel::Account(account) => {
                format!("/v2/stream?channel=account&account={}", id_hex(account))
            }
            Channel::Book(market) => format!("/v2/stream?channel=book&market={}", id_hex(market)),
            Channel::Trades(market) => {
                format!("/v2/stream?channel=trades&market={}", id_hex(market))
            }
        };
        if let Some(cursor) = from {
            path.push_str("&from=");
            path.push_str(cursor);
        }
        path
    }
}

pub struct Session {
    pub genesis: Genesis,
    pub trust: Trust,
    pub seats: Vec<SeatEndpoint>,
    dir: PathBuf,
    /// Set once: loaded with the folder, or created later.
    key: OnceLock<SecretKey>,
    outbox: Mutex<Outbox>,
    /// Held from choosing a nonce to storing the signed action, so two
    /// submissions never sign the same nonce. The outbox itself is locked
    /// only for single reads and writes, so it stays readable meanwhile.
    signing: Mutex<()>,
    pub timeout: Duration,
}

/// A short name for the network of `trust`: the folder its account lives in.
pub fn network_tag(trust: &Trust) -> String {
    let mut bytes = trust.domain.to_vec();
    bytes.extend_from_slice(trust.seats.root());
    hex(&sha256(&bytes)[..8])
}

fn now_unix() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |elapsed| elapsed.as_secs())
}

impl Session {
    /// Checks the genesis document (every seat proves possession of its
    /// key) and the pins, and opens the account folder `dir` with its
    /// outbox and, when it has one, its key. Nothing is sent. Seats must be
    /// on this computer: remote access is not decided (hive/README.md).
    pub fn open(genesis_json: &str, pins_json: &str, dir: &Path) -> Result<Self, Error> {
        let genesis = Genesis::parse(genesis_json)?;
        let trust = Trust::from_genesis(&genesis)?;
        let pins = parse_pins(pins_json, genesis.seats.len())?;
        let mut seats = Vec::with_capacity(genesis.seats.len());
        for (seat, pin) in genesis.seats.iter().zip(pins) {
            if !seat.api_address.is_loopback() {
                return Err(Error::Invalid(format!(
                    "seat {} is at {}: B3 Hive connects to seats on this computer only",
                    seat.index, seat.api_address
                )));
            }
            seats.push(SeatEndpoint {
                seat: seat.index,
                address: seat.api_address,
                port: seat.api_port,
                pin,
            });
        }
        let outbox = Outbox::open(dir, &trust.profile)?;
        let key = OnceLock::new();
        if let Some(loaded) = load_key(dir)? {
            let _ = key.set(loaded);
        }
        Ok(Self {
            genesis,
            trust,
            seats,
            dir: dir.to_owned(),
            key,
            outbox: Mutex::new(outbox),
            signing: Mutex::new(()),
            timeout: Duration::from_secs(5),
        })
    }

    /// The account of this folder's key.
    pub fn account(&self) -> Option<H256> {
        self.key.get().map(|key| account_of(&key.public()))
    }

    /// Creates the folder's key. An existing key is never replaced.
    pub fn create_key(&self) -> Result<H256, Error> {
        let _signing = self
            .signing
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        if self.key.get().is_some() {
            return Err(Error::Invalid("this account already has a key".into()));
        }
        let key = create_key(&self.dir)?;
        let account = account_of(&key.public());
        let _ = self.key.set(key);
        Ok(account)
    }

    fn outbox(&self) -> std::sync::MutexGuard<'_, Outbox> {
        self.outbox
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
    }

    /// Runs `ask` for every seat at once and returns the answers in seat order.
    fn each_seat<T: Send>(&self, ask: impl Fn(&SeatEndpoint) -> T + Sync) -> Vec<T> {
        std::thread::scope(|scope| {
            let asks: Vec<_> = self
                .seats
                .iter()
                .map(|seat| scope.spawn(|| ask(seat)))
                .collect();
            asks.into_iter()
                .map(|ask| ask.join().expect("a seat request panicked"))
                .collect()
        })
    }

    /// A JSON reply; a status other than 200 is an error in the seat's words.
    fn json(
        &self,
        seat: &SeatEndpoint,
        method: &str,
        path: &str,
        body: Option<&[u8]>,
    ) -> Result<Value, Error> {
        let response = request(seat, method, path, body, self.timeout)?;
        let value: Value = serde_json::from_slice(&response.body).map_err(|_| {
            Error::Transport(format!(
                "seat {}: HTTP {} without JSON",
                seat.seat, response.status
            ))
        })?;
        if response.status != 200 {
            let (code, reason) = parse_error(&value).unwrap_or_default();
            return Err(Error::Transport(format!(
                "seat {} answered HTTP {} {code}: {reason}",
                seat.seat, response.status
            )));
        }
        Ok(value)
    }

    /// Every seat's own diagnostics. Not certified.
    pub fn health(&self) -> Vec<(u32, Result<Health, Error>)> {
        self.each_seat(|seat| {
            (
                seat.seat,
                self.json(seat, "GET", "/v2/health", None)
                    .and_then(|v| parse_health(&v)),
            )
        })
    }

    /// Asks every seat for `account` and verifies each answer.
    pub fn state(&self, account: &H256) -> Vec<SeatState> {
        let path = format!("/v2/state/{}", id_hex(account));
        self.each_seat(|seat| {
            let answer = self.json(seat, "GET", &path, None).and_then(|value| {
                let reply = parse_state(&value, &self.trust.profile)?;
                self.trust.verify_head(&reply.head)?;
                let leaf = match &reply.witness {
                    Some(witness) => Some(
                        self.trust
                            .verify_account(&reply.head, witness, account)?
                            .clone(),
                    ),
                    None => None,
                };
                Ok((reply.head.envelope.header.height, leaf))
            });
            match answer {
                Ok((height, leaf)) => SeatState {
                    seat: seat.seat,
                    height: Some(height),
                    leaf,
                    error: None,
                },
                Err(error) => SeatState {
                    seat: seat.seat,
                    height: None,
                    leaf: None,
                    error: Some(error.to_string()),
                },
            }
        })
    }

    /// Signs `payload`, stores the exact bytes and posts them to every seat.
    /// The caller has the user's approval for exactly this payload.
    pub fn submit(&self, payload: &Payload, max_delay: u16) -> Result<Submitted, Error> {
        let mut submitted = self.sign_and_store(payload, max_delay)?;
        submitted.admissions = self.resend(&submitted.label)?;
        Ok(submitted)
    }

    /// The first half of `submit`: the action is signed and durable in the
    /// outbox, and nothing was sent yet (`admissions` is empty). This is
    /// also where an action stands when no seat could be reached afterwards.
    pub fn sign_and_store(&self, payload: &Payload, max_delay: u16) -> Result<Submitted, Error> {
        let key = self
            .key
            .get()
            .ok_or(Error::Invalid("this account has no key yet".into()))?;
        let account = account_of(&key.public());
        if max_delay == 0 || u32::from(max_delay) > self.trust.profile.max_delay_slots {
            return Err(Error::Invalid(
                "the action's window is outside what the network allows".into(),
            ));
        }
        let _signing = self
            .signing
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        let states = self.state(&account);
        let heights: Vec<u64> = states.iter().filter_map(|state| state.height).collect();
        let (Some(&best), Some(&lowest)) = (heights.iter().max(), heights.iter().min()) else {
            let why: Vec<String> = states
                .iter()
                .filter_map(|state| state.error.clone())
                .collect();
            return Err(Error::Transport(format!(
                "no seat answered with a verified head: {}",
                why.join("; ")
            )));
        };
        let ref_slot = lowest.max(best.saturating_sub(REF_SLOT_LAG));
        let leaf_nonce = states
            .iter()
            .filter(|state| state.leaf.is_some())
            .max_by_key(|state| state.height)
            .and_then(|state| state.leaf.as_ref())
            .and_then(|leaf| leaf.used_nonces.last().copied());
        let signed_nonce = self.outbox().highest_nonce(&account);
        let nonce = leaf_nonce
            .max(signed_nonce)
            .unwrap_or(0)
            .checked_add(1)
            .ok_or(Error::Invalid("the account's nonces are used up".into()))?;

        let mut action = Action {
            domain: self.trust.domain,
            signer: key.public(),
            nonce,
            ref_slot,
            max_delay,
            kind: payload.kind(),
            payload: payload.encode()?,
            sig: [0; 64],
        };
        let action_id = action.id();
        action.sig = key.sign(&action_id);
        let bytes = action.encode();
        // What is stored and sent must be what a seat will decode.
        if Action::decode(&bytes, &self.trust.profile)? != action {
            return Err(Error::Invalid(
                "the signed action does not decode to itself".into(),
            ));
        }
        let kind = match payload {
            Payload::Place { .. } => "PLACE",
            Payload::Cancel { .. } => "CANCEL",
            Payload::CancelAll => "CANCEL_ALL",
            Payload::Transfer { .. } => "TRANSFER",
        };
        let label = format!("{kind}-{nonce}");
        // Durable before the first byte leaves.
        self.outbox().add(&label, now_unix(), &bytes)?;
        Ok(Submitted {
            label,
            action_id,
            nonce,
            ref_slot,
            max_delay,
            admissions: Vec::new(),
        })
    }

    /// Posts exact action bytes to every seat.
    fn post(&self, bytes: &[u8]) -> Vec<SeatAdmission> {
        self.each_seat(|seat| {
            let answer = request(seat, "POST", "/v2/actions", Some(bytes), self.timeout).and_then(
                |response| {
                    let value: Value = serde_json::from_slice(&response.body).map_err(|_| {
                        Error::Transport(format!("HTTP {} without JSON", response.status))
                    })?;
                    match parse_admission(&value) {
                        Ok(reply) => Ok((reply.code, reply.reason)),
                        Err(_) => {
                            let (code, reason) = parse_error(&value).unwrap_or_default();
                            Err(Error::Transport(format!(
                                "HTTP {} {code}: {reason}",
                                response.status
                            )))
                        }
                    }
                },
            );
            match answer {
                Ok((code, reason)) => SeatAdmission {
                    seat: seat.seat,
                    code,
                    reason,
                },
                Err(error) => SeatAdmission {
                    seat: seat.seat,
                    code: String::new(),
                    reason: error.to_string(),
                },
            }
        })
    }

    /// Posts the stored bytes of `label` again. Never signs.
    pub fn resend(&self, label: &str) -> Result<Vec<SeatAdmission>, Error> {
        let bytes = self.entry(label)?.bytes;
        Ok(self.post(&bytes))
    }

    pub fn entry(&self, label: &str) -> Result<Entry, Error> {
        self.outbox()
            .find(label)
            .cloned()
            .ok_or(Error::Invalid("no outbox entry with this label".into()))
    }

    pub fn entries(&self) -> Vec<Entry> {
        self.outbox().entries().to_vec()
    }

    pub fn seen_fills(&self) -> Vec<SeenFill> {
        self.outbox().fills().to_vec()
    }

    /// Notes fills a trade stream listed for this outbox's own orders.
    pub fn note_fills(&self, fills: &[SeenFill]) -> Result<bool, Error> {
        let mut outbox = self.outbox();
        let own: Vec<H256> = outbox
            .entries()
            .iter()
            .filter(|entry| {
                matches!(
                    Payload::decode(entry.action.kind, &entry.action.payload),
                    Ok(Some(Payload::Place { .. }))
                )
            })
            .map(|entry| entry.action.id())
            .collect();
        let mut noted = false;
        for fill in fills.iter().filter(|fill| own.contains(&fill.order)) {
            noted |= outbox.note_fill(fill.clone())?;
        }
        Ok(noted)
    }

    /// Asks every seat once for the status of `label` and records a
    /// certified end. An end that is already recorded is returned as it is.
    pub fn settle(&self, label: &str) -> Result<Progress, Error> {
        let entry = self.entry(label)?;
        match entry.result {
            Some(Certified::Included {
                height,
                index,
                outcome,
            }) => {
                return Ok(Progress::Included {
                    seat: None,
                    height,
                    index,
                    outcome,
                })
            }
            Some(Certified::Expired { height }) => {
                return Ok(Progress::Expired { seat: None, height })
            }
            None => {}
        }
        let id = entry.action.id();
        enum Answer {
            Included(u64, u32, Outcome),
            Expired(u64),
            Pending,
            Terminal,
            RefusedClaim(String),
            Other(String),
        }
        let answers = self.each_seat(|seat| {
            let reply = match self
                .json(seat, "POST", "/v2/actions/status", Some(&entry.bytes))
                .and_then(|value| parse_status(&value, &self.trust.profile))
            {
                Ok(reply) => reply,
                Err(error) => return (seat.seat, Answer::Other(error.to_string())),
            };
            let answer = match (
                reply.status.as_str(),
                &reply.included,
                &reply.head,
                &reply.expiry,
            ) {
                ("INCLUDED", Some(included), _, _) => {
                    match self.trust.verify_included(included, &id) {
                        Ok(outcome) => Answer::Included(
                            included.ref_height,
                            included.ref_index as u32,
                            outcome.clone(),
                        ),
                        Err(error) => Answer::RefusedClaim(error.to_string()),
                    }
                }
                ("EXPIRED", _, Some(head), Some(witness)) => {
                    match self.trust.verify_expired(&entry.action, head, witness) {
                        Ok(()) => Answer::Expired(head.envelope.header.height),
                        Err(error) => Answer::RefusedClaim(error.to_string()),
                    }
                }
                ("INCLUDED" | "EXPIRED", ..) => {
                    Answer::RefusedClaim("the answer carries no proof".into())
                }
                ("PENDING", ..) => Answer::Pending,
                ("UNKNOWN", ..) if reply.reason == UNKNOWN_TERMINAL_UNPROVEN => Answer::Terminal,
                _ => Answer::Other(reply.reason),
            };
            (seat.seat, answer)
        });

        let mut pending = Vec::new();
        let mut refused_claims = 0;
        let mut terminal = 0;
        let mut details = Vec::new();
        let mut end = None;
        for (seat, answer) in answers {
            match answer {
                Answer::Included(height, index, outcome) if end.is_none() => {
                    end = Some((
                        seat,
                        Certified::Included {
                            height,
                            index,
                            outcome,
                        },
                    ));
                }
                Answer::Expired(height) if end.is_none() => {
                    end = Some((seat, Certified::Expired { height }))
                }
                Answer::Included(..) | Answer::Expired(..) => {}
                Answer::Pending => pending.push(seat),
                Answer::Terminal => terminal += 1,
                Answer::RefusedClaim(why) => {
                    refused_claims += 1;
                    details.push(format!("seat {seat}: unverified claim ({why})"));
                }
                Answer::Other(why) if !why.is_empty() => {
                    details.push(format!("seat {seat}: {why}"))
                }
                Answer::Other(_) => {}
            }
        }
        if let Some((seat, result)) = end {
            let mut outbox = self.outbox();
            // Another settle of the same label may have recorded it meanwhile.
            if outbox
                .find(label)
                .is_some_and(|entry| entry.result.is_none())
            {
                outbox.record(label, result.clone())?;
            }
            return Ok(match result {
                Certified::Included {
                    height,
                    index,
                    outcome,
                } => Progress::Included {
                    seat: Some(seat),
                    height,
                    index,
                    outcome,
                },
                Certified::Expired { height } => Progress::Expired {
                    seat: Some(seat),
                    height,
                },
            });
        }
        if terminal == self.seats.len() {
            return Ok(Progress::Failed);
        }
        Ok(Progress::Waiting {
            pending,
            refused_claims,
            detail: details.join("; "),
        })
    }

    /// Follows one channel until `stop` is set: the first seat that
    /// answers, the next one when it breaks, and a resubscription from the
    /// last cursor on the same seat after a gap. Slot events are verified
    /// here; a slot event that does not verify is dropped and the seat left.
    pub fn follow(&self, channel: Channel, stop: &AtomicBool, mut sink: impl FnMut(StreamUpdate)) {
        let mut seat_index = 0;
        let mut cursor: Option<String> = None;
        while !stop.load(Ordering::Relaxed) {
            let seat = &self.seats[seat_index % self.seats.len()];
            let mut live = false;
            let mut gap = false;
            let mut bad: Option<String> = None;
            let path = channel.path(cursor.as_deref());
            let ended = stream(seat, &path, self.timeout, STREAM_IDLE, stop, |event| {
                if !live {
                    live = true;
                    sink(StreamUpdate::Link {
                        seat: seat.seat,
                        live: true,
                        detail: String::new(),
                    });
                }
                match event.event.as_str() {
                    "heartbeat" => return true,
                    "gap" => {
                        gap = true;
                        return false;
                    }
                    _ => {}
                }
                let parsed = serde_json::from_str::<Value>(&event.data)
                    .map_err(|_| Error::Decode("a stream event is not JSON"))
                    .and_then(|value| parse_stream_event(&value, &self.trust.profile));
                let event = match parsed {
                    Ok(Some(event)) => event,
                    Ok(None) => return true,
                    Err(error) => {
                        bad = Some(error.to_string());
                        return false;
                    }
                };
                let verified = match &event.body {
                    StreamBody::Slot { envelope, .. } => {
                        if let Err(error) = self.trust.verify_envelope(envelope) {
                            bad = Some(error.to_string());
                            return false;
                        }
                        true
                    }
                    _ => false,
                };
                cursor = Some(event.cursor.clone());
                sink(StreamUpdate::Event {
                    seat: seat.seat,
                    verified,
                    event,
                });
                true
            });
            if stop.load(Ordering::Relaxed) {
                return;
            }
            let detail = match (&ended, bad, gap) {
                (_, Some(bad), _) => {
                    format!("seat {} sent an event that was refused: {bad}", seat.seat)
                }
                (_, None, true) => "the stream fell behind and is resubscribing".to_owned(),
                (Ok(StreamEnd::Closed), ..) => format!("seat {} closed the stream", seat.seat),
                (Ok(StreamEnd::Stopped), ..) => String::new(),
                (Err(error), ..) => error.to_string(),
            };
            sink(StreamUpdate::Link {
                seat: seat.seat,
                live: false,
                detail,
            });
            if !gap {
                // Another seat, from its own snapshot: a cursor names one
                // seat's publisher and means nothing to another.
                seat_index += 1;
                cursor = None;
            }
            let wake = std::time::Instant::now() + STREAM_RETRY;
            while std::time::Instant::now() < wake && !stop.load(Ordering::Relaxed) {
                std::thread::sleep(Duration::from_millis(100));
            }
        }
    }
}
