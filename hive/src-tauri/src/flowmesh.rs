//! The Trade page's FlowMesh calls: markets, their verified book, trades and
//! clearings, this wallet's trading account, and the orders and
//! cancellations it approves. Every FlowMesh call goes through here, so a protocol change touches this adapter
//! and its tests rather than the page.
//!
//! Amounts stay exact: prices in B3 atoms per base unit, quantities in base
//! units and B3 in atoms, all as integer strings. The page formats them with
//! BigInt and never parses them as floating point.

use serde::{Deserialize, Serialize};
use serde_json::{json, Map, Value};

use crate::rpc::{Client, RpcError};

/// The longest a live read waits for a new head, as the node allows.
const WATCH_MS: u64 = 1500;
/// Book levels per side.
const BOOK_DEPTH: u64 = 20;
/// Certified history entries per read, the most the node returns.
const HISTORY_LIMIT: u64 = 100;
/// Clearing ticks per read, the most the node returns.
const TICK_LIMIT: u64 = 256;

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct MarketSummary {
    pub market_id: String,
    pub base_asset_id: String,
    pub running: bool,
    pub paused: bool,
    /// "none" or the node's fail-closed halt reason.
    pub halt: String,
    pub error: String,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Metadata {
    /// Display decimals, when the node has verified them.
    pub decimals: Option<u32>,
    pub ticker: String,
    pub name: String,
    /// A configured unbacked test asset.
    pub test_only: bool,
}

/// Where the data came from and what was checked; shown, never relied on.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Verification {
    /// local_engine or remote_endpoint
    pub source: String,
    pub endpoint: String,
    pub certificate_verified: bool,
    pub account_state_verified: bool,
    pub history_endpoint_reported: bool,
    pub event_gap: bool,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Status {
    /// At least one quorum-certified microblock exists.
    pub certified: bool,
    pub head: String,
    pub next_sequence: u64,
    pub anchor_height: i64,
    pub active_seats: u64,
    pub quorum_required: u64,
    pub running: bool,
    pub paused: bool,
    pub reconciling: bool,
    pub pending_handoff: bool,
    pub halt: String,
    pub error: String,
    /// Queued, uncertified actions: not fills.
    pub pending_actions: u64,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Point {
    pub price: String,
    pub quantity: String,
}

/// A standing curve: one per account and side, not a price-time order.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Order {
    /// bid or ask
    pub side: String,
    /// open or partially-filled
    pub status: String,
    pub filled: String,
    pub remaining: String,
    /// B3 atoms for a bid, base units for an ask.
    pub reserved: String,
    pub points: Vec<Point>,
}

/// This wallet's certified trading account.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Account {
    pub account_id: String,
    pub next_sequence: u64,
    pub base_available: String,
    pub base_reserved: String,
    pub b3_available: String,
    pub b3_reserved: String,
    pub orders: Vec<Order>,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Level {
    pub low: String,
    pub high: String,
    pub size: String,
    pub cumulative: String,
    pub curves: u64,
    /// This wallet's part of size.
    pub own: String,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Side {
    pub available: bool,
    /// More levels follow the last one.
    pub more: bool,
    pub levels: Vec<Level>,
}

/// The node's exact price levels of every verified standing curve. Not
/// certified, and queued actions are not in it.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Book {
    pub available: bool,
    pub reason: String,
    /// The head these levels were computed for.
    pub head: String,
    pub best_bid: Option<String>,
    pub best_ask: Option<String>,
    pub bids: Option<Side>,
    pub asks: Option<Side>,
    /// What the next auction would do with the standing curves alone.
    pub standing: Option<Clearing>,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Clearing {
    pub cleared: bool,
    pub price: String,
    pub volume: String,
}

/// A certified microblock's auction. The protocol has no execution time.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Tick {
    pub sequence: u64,
    /// execution, settlement or handoff
    pub kind: String,
    pub cleared: bool,
    pub price: String,
    pub volume: String,
}

/// A certified auction that matched.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Trade {
    pub sequence: u64,
    pub price: String,
    pub quantity: String,
    pub notional: String,
    pub fee: String,
    /// This wallet's fills; None when the node does not know them, which is
    /// not the same as no fill.
    pub own_bought: Option<String>,
    pub own_sold: Option<String>,
    /// When the serving node applied it, by its own clock: Unix ms, never a
    /// certified time. None when it was applied late or not observed.
    pub observed_ms: Option<u64>,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct MarketView {
    pub market_id: String,
    pub base_asset_id: String,
    /// The heavy parts are left out because the head has not moved.
    pub unchanged: bool,
    /// Order input steps: base units and B3 atoms per base unit.
    pub quantity_lot: String,
    pub price_tick: String,
    pub metadata: Option<Metadata>,
    pub verification: Verification,
    pub status: Status,
    pub account: Option<Account>,
    pub book: Option<Book>,
    pub trades: Option<Vec<Trade>>,
    pub ticks: Vec<Tick>,
    /// Ticks before the first returned one were dropped by the node.
    pub ticks_gap: bool,
    /// The live read waited for a change; otherwise pace the next one.
    pub waited: bool,
}

pub fn valid_id(text: &str) -> bool {
    text.len() == 64 && text.bytes().all(|b| b.is_ascii_hexdigit())
}

pub fn markets(client: &Client, wallet: &str) -> Result<Vec<MarketSummary>, RpcError> {
    let list = client.call_wallet(wallet, "listflowmeshmarkets", json!([]))?;
    Ok(list
        .as_array()
        .map(|all| {
            all.iter()
                .filter(|m| m["available"].as_bool().unwrap_or(false))
                .map(market_summary)
                .collect()
        })
        .unwrap_or_default())
}

fn market_summary(m: &Value) -> MarketSummary {
    MarketSummary {
        market_id: text(&m["market_id"]),
        base_asset_id: text(&m["base_asset_id"]),
        running: m["running"].as_bool().unwrap_or(false),
        paused: m["paused"].as_bool().unwrap_or(false),
        halt: text(&m["halt"]),
        error: text(&m["error"]),
    }
}

/// What the page asks for: a fresh read, or a live one that waits for the
/// head to move from `known_head`, and the clearing ticks after the last one
/// it holds.
#[derive(Debug, Clone, Default)]
pub struct Query {
    pub known_head: Option<String>,
    pub watch: bool,
    pub ticks_after: Option<u64>,
}

pub fn query_options(query: &Query) -> Value {
    let mut options = Map::new();
    options.insert("limit".into(), json!(HISTORY_LIMIT));
    options.insert("curve_limit".into(), json!(1));
    options.insert("levels".into(), json!({ "depth": BOOK_DEPTH }));
    options.insert("observed_time".into(), json!(true));
    let mut ticks = Map::new();
    ticks.insert("limit".into(), json!(TICK_LIMIT));
    if let Some(after) = query.ticks_after {
        ticks.insert("after_sequence".into(), json!(after));
    }
    options.insert("ticks".into(), Value::Object(ticks));
    if let Some(head) = &query.known_head {
        options.insert("known_head".into(), json!(head));
    }
    if query.watch {
        options.insert("watch_ms".into(), json!(WATCH_MS));
    }
    Value::Object(options)
}

pub fn market(
    client: &Client,
    wallet: &str,
    market_id: &str,
    query: &Query,
) -> Result<MarketView, String> {
    if !valid_id(market_id) {
        return Err("That is not a market id.".into());
    }
    if let Some(head) = &query.known_head {
        if !valid_id(head) {
            return Err("That is not a microblock hash.".into());
        }
    }
    let data = client
        .call_wallet(
            wallet,
            "getflowmeshmarketdata",
            json!([market_id, query_options(query)]),
        )
        .map_err(|e| e.to_string())?;
    Ok(market_view(&data))
}

pub fn market_view(d: &Value) -> MarketView {
    let s = &d["snapshot"];
    let v = &d["verification"];
    let ticks = &d["clearing_ticks"];
    MarketView {
        market_id: text(&d["market_id"]),
        base_asset_id: text(&d["base_asset_id"]),
        unchanged: d["unchanged"].as_bool().unwrap_or(false),
        quantity_lot: int(&d["quantity_lot_raw"]),
        price_tick: int(&d["price_tick_raw"]),
        metadata: d.get("base_metadata").map(|m| Metadata {
            decimals: if m["known"].as_bool().unwrap_or(false) {
                m["decimals"].as_u64().map(|d| d as u32)
            } else {
                None
            },
            ticker: text(&m["ticker"]),
            name: text(&m["name"]),
            test_only: m["test_only"].as_bool().unwrap_or(false),
        }),
        verification: Verification {
            source: text(&v["source"]),
            endpoint: text(&v["endpoint"]),
            certificate_verified: v["certificate_verified"].as_bool().unwrap_or(false),
            account_state_verified: v["account_state_verified"].as_bool().unwrap_or(false),
            history_endpoint_reported: v["history_endpoint_reported"].as_bool().unwrap_or(true),
            event_gap: v["event_gap"].as_bool().unwrap_or(false),
        },
        status: Status {
            certified: s["certified"].as_bool().unwrap_or(false),
            head: text(&s["last_microblock_hash"]),
            next_sequence: s["next_microblock_sequence"].as_u64().unwrap_or(0),
            anchor_height: s["anchor_height"].as_i64().unwrap_or(-1),
            active_seats: s["active_seats"].as_u64().unwrap_or(0),
            quorum_required: s["quorum_required"].as_u64().unwrap_or(0),
            running: s["running"].as_bool().unwrap_or(false),
            paused: s["paused"].as_bool().unwrap_or(false),
            reconciling: s["chain_reconciling"].as_bool().unwrap_or(false),
            pending_handoff: s["pending_handoff"].as_bool().unwrap_or(false),
            halt: text(&s["halt"]),
            error: text(&s["error"]),
            pending_actions: s["pending_actions"].as_u64().unwrap_or(0),
        },
        account: d.get("account").map(account),
        book: d.get("book_levels").map(book),
        trades: d.get("history").map(|h| trades(h, &d["observed_time"])),
        ticks: ticks["ticks"]
            .as_array()
            .map(|all| all.iter().map(tick).collect())
            .unwrap_or_default(),
        ticks_gap: ticks["gap"].as_bool().unwrap_or(false),
        waited: d["watch"]["waited"].as_bool().unwrap_or(false),
    }
}

fn account(a: &Value) -> Account {
    Account {
        account_id: text(&a["account_id"]),
        next_sequence: a["next_sequence"].as_u64().unwrap_or(0),
        base_available: int(&a["base_available"]),
        base_reserved: int(&a["base_reserved"]),
        b3_available: int(&a["b3_available_atoms"]),
        b3_reserved: int(&a["b3_reserved_atoms"]),
        orders: a["curves"]
            .as_array()
            .map(|all| {
                all.iter()
                    .map(|c| Order {
                        side: text(&c["side"]),
                        status: text(&c["status"]),
                        filled: int(&c["filled_quantity"]),
                        remaining: int(&c["remaining_quantity"]),
                        reserved: int(&c["reserved_amount"]),
                        points: c["points"]
                            .as_array()
                            .map(|ps| {
                                ps.iter()
                                    .map(|p| Point {
                                        price: int(&p["price"]),
                                        quantity: int(&p["quantity"]),
                                    })
                                    .collect()
                            })
                            .unwrap_or_default(),
                    })
                    .collect()
            })
            .unwrap_or_default(),
    }
}

fn book(b: &Value) -> Book {
    let side = |s: &Value| -> Option<Side> {
        s.as_object()?;
        Some(Side {
            available: s["available"].as_bool().unwrap_or(false),
            more: s["more"].as_bool().unwrap_or(false),
            levels: s["levels"]
                .as_array()
                .map(|all| {
                    all.iter()
                        .map(|l| Level {
                            low: int(&l["low"]),
                            high: int(&l["high"]),
                            size: int(&l["size"]),
                            cumulative: int(&l["cumulative"]),
                            curves: l["curves"].as_u64().unwrap_or(0),
                            own: int(&l["own"]),
                        })
                        .collect()
                })
                .unwrap_or_default(),
        })
    };
    let standing = &b["standing"];
    Book {
        available: b["available"].as_bool().unwrap_or(false),
        reason: text(&b["unavailable_reason"]),
        head: text(&b["head"]),
        best_bid: b.get("best_bid").map(int),
        best_ask: b.get("best_ask").map(int),
        bids: side(&b["bids"]),
        asks: side(&b["asks"]),
        standing: standing
            .get("available")
            .and_then(Value::as_bool)
            .filter(|&ok| ok)
            .map(|_| Clearing {
                cleared: standing["cleared"].as_bool().unwrap_or(false),
                price: int(&standing["price"]),
                volume: int(&standing["volume"]),
            }),
    }
}

fn tick(t: &Value) -> Tick {
    Tick {
        sequence: t["sequence"].as_u64().unwrap_or(0),
        kind: text(&t["kind"]),
        cleared: t["cleared"].as_bool().unwrap_or(false),
        price: int(&t["price"]),
        volume: int(&t["volume"]),
    }
}

/// Matched auctions, newest first, with the live observed time of each.
fn trades(history: &Value, observed: &Value) -> Vec<Trade> {
    let live = |sequence: u64| -> Option<u64> {
        observed["rows"].as_array()?.iter().find_map(|row| {
            (row["sequence"].as_u64() == Some(sequence) && row["mode"] == "live")
                .then(|| row["observed_ms"].as_u64())
                .flatten()
        })
    };
    history["entries"]
        .as_array()
        .map(|all| {
            all.iter()
                .filter(|e| e["cleared"].as_bool().unwrap_or(false))
                .map(|e| {
                    let sequence = e["sequence"].as_u64().unwrap_or(0);
                    let known = e["account_fills_known"].as_bool().unwrap_or(false);
                    Trade {
                        sequence,
                        price: int(&e["price"]),
                        quantity: int(&e["quantity"]),
                        notional: int(&e["notional_atoms"]),
                        fee: int(&e["fee_atoms"]),
                        own_bought: known.then(|| int(&e["account_bid_fill"])),
                        own_sold: known.then(|| int(&e["account_ask_fill"])),
                        observed_ms: live(sequence),
                    }
                })
                .collect()
        })
        .unwrap_or_default()
}

// ---- orders -----------------------------------------------------------------

/// How long a submission waits for its certified inclusion, where the
/// endpoint can report it: the node's most (FLOWMESH_CLIENT_ACTION_WAIT_MAX),
/// which refuses more before signing. Nothing is signed or sent again to wait.
const SUBMIT_WAIT_MS: u64 = 2500;
/// A ladder holds at most this many prices (FLOWMESH_LADDER_MAX_STEPS).
pub const LADDER_MAX_STEPS: usize = 4;
/// After this long without certification a request may be acknowledged as
/// uncertain (doc/flowmesh-trading-ui.md: submissions pause after 30 s).
pub const PENDING_ACK_MS: u64 = 30_000;

/// A limit order or ladder as the approval window shows it, and exactly as
/// it is signed: nothing in it changes after the review opens.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct OrderDraft {
    pub chain: String,
    pub wallet: String,
    pub market_id: String,
    pub base_asset_id: String,
    pub metadata: Metadata,
    pub price_tick: String,
    /// bid (buy the token with B3) or ask (sell it for B3)
    pub side: String,
    /// One price for a limit order, up to four for a ladder, best first: a
    /// bid's highest, an ask's lowest.
    pub steps: Vec<Point>,
    /// The account sequence it is signed with.
    pub sequence: u64,
    /// What it reserves: B3 atoms for a bid, base units for an ask.
    pub reserve: String,
    /// What it may reserve: the free balance plus what the replaced order
    /// holds.
    pub available: String,
    /// This account's open order on that side, which it replaces.
    pub replaces: Option<Order>,
    /// Verified by an endpoint rather than this node's own engine.
    pub remote: bool,
}

/// Cancelling this wallet's open order on one side.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct CancelDraft {
    pub chain: String,
    pub wallet: String,
    pub market_id: String,
    pub base_asset_id: String,
    pub metadata: Metadata,
    pub price_tick: String,
    pub side: String,
    pub sequence: u64,
    pub order: Order,
    pub remote: bool,
}

/// A verified per-action outcome, when the node has one.
#[derive(Debug, Clone, Default, PartialEq, Serialize)]
pub struct Outcome {
    /// applied, rejected, included_not_executed or not_executed; None while
    /// no outcome is verified.
    pub status: Option<String>,
    pub text: String,
    /// Base units filled in the including entry. None when it is not an
    /// order or nothing is verified: never a guessed zero.
    pub fill_quantity: Option<String>,
    /// The entry's uniform clearing price, when its auction cleared.
    pub fill_price: Option<String>,
}

/// What the node said about a submission. Accepted is not certified, and
/// certified is not a fill.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Receipt {
    pub action_id: String,
    pub accepted: bool,
    /// queued, admitted, rejected, unknown or certified_inclusion
    pub state: String,
    /// signed, sent, queued, admitted, certified, outcome_verified,
    /// not_executed, refused or unknown
    pub stage: String,
    pub reason: String,
    pub outcome: Outcome,
}

/// One of this wallet's retained requests in a market, newest first.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Request {
    pub action_id: String,
    /// order, cancel or other (deposits and withdrawals)
    pub kind: String,
    pub side: Option<String>,
    pub points: Vec<Point>,
    pub sequence: Option<u64>,
    pub submitted_ms: u64,
    pub state: String,
    pub stage: String,
    pub reason: String,
    pub certified: bool,
    pub outcome: Outcome,
    /// Below the certified account's next sequence: the market has taken it
    /// in, or another action used its sequence.
    pub settled: bool,
    /// Blocks new actions until it settles or is acknowledged.
    pub pending: bool,
    /// It may be acknowledged: its delivery is unknown, or it has waited
    /// longer than PENDING_ACK_MS.
    pub uncertain: bool,
}

fn side_of(side: &str) -> Result<&'static str, String> {
    match side {
        "bid" => Ok("bid"),
        "ask" => Ok("ask"),
        _ => Err("An order is a buy (bid) or a sell (ask).".into()),
    }
}

fn integer(text: &str, what: &str) -> Result<u64, String> {
    if text.is_empty() || !text.bytes().all(|b| b.is_ascii_digit()) {
        return Err(format!("The {what} is not a whole number of base units."));
    }
    text.parse::<u64>()
        .map_err(|_| format!("The {what} is too large."))
}

fn now_ms() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

/// A market's current state for a review: units, status and this wallet's
/// account, with the smallest book and history the node allows.
fn fresh(client: &Client, wallet: &str, market_id: &str) -> Result<MarketView, String> {
    if !valid_id(market_id) {
        return Err("That is not a market id.".into());
    }
    let data = client
        .call_wallet(
            wallet,
            "getflowmeshmarketdata",
            json!([market_id, {"limit": 1, "curve_limit": 1}]),
        )
        .map_err(|e| e.to_string())?;
    Ok(market_view(&data))
}

/// Refuses a new action while the market cannot take one.
fn tradable(view: &MarketView) -> Result<(), String> {
    let s = &view.status;
    if !s.certified {
        return Err("This market has no certified state yet.".into());
    }
    if !s.halt.is_empty() && s.halt != "none" {
        return Err(format!("This market is halted ({}).", s.halt));
    }
    if s.paused {
        return Err("Trading in this market is paused.".into());
    }
    if s.reconciling {
        return Err("The node is catching up with the B3 chain. Try again in a moment.".into());
    }
    Ok(())
}

/// Marks which requests still block new ones. A request the certified
/// account has not taken in yet would compete with a new one for the same
/// sequence, so nothing new is drafted until it settles, unless the user
/// acknowledged it as uncertain. Acknowledging never retries it.
fn classify(list: &mut [Request], next_sequence: u64, now_ms: u64, acknowledged: &[String]) {
    for r in list {
        r.settled = r.sequence.is_some_and(|s| s < next_sequence);
        let open = r.kind != "other" && !r.settled && !r.certified && r.state != "rejected";
        r.uncertain = open
            && (r.state == "unknown" || now_ms.saturating_sub(r.submitted_ms) >= PENDING_ACK_MS);
        r.pending = open && !acknowledged.contains(&r.action_id);
    }
}

/// This wallet's requests in a market, with what still blocks new ones.
pub fn requests_view(
    client: &Client,
    wallet: &str,
    market_id: &str,
    acknowledged: &[String],
) -> Result<Vec<Request>, String> {
    let view = fresh(client, wallet, market_id)?;
    let next = view.account.map(|a| a.next_sequence).unwrap_or(0);
    let mut list = requests(client, wallet, market_id).map_err(|e| e.to_string())?;
    classify(&mut list, next, now_ms(), acknowledged);
    Ok(list)
}

fn check_pending(
    client: &Client,
    wallet: &str,
    market_id: &str,
    next_sequence: u64,
    acknowledged: &[String],
) -> Result<(), String> {
    let mut list = requests(client, wallet, market_id).map_err(|e| e.to_string())?;
    classify(&mut list, next_sequence, now_ms(), acknowledged);
    if list.iter().any(|r| r.pending) {
        return Err("Your previous request is not certified yet. Wait for it before sending another, so the two do not compete.".into());
    }
    Ok(())
}

/// Whether `action_id` may be acknowledged as uncertain now.
pub fn can_acknowledge(
    client: &Client,
    wallet: &str,
    market_id: &str,
    action_id: &str,
) -> Result<(), String> {
    let list = requests_view(client, wallet, market_id, &[])?;
    match list.iter().find(|r| r.action_id == action_id) {
        Some(r) if r.uncertain => Ok(()),
        Some(_) => Err("That request is not uncertain; wait for it.".into()),
        None => Err("That request is not this wallet's in this market.".into()),
    }
}

/// Just before an approved action is signed: the market must still take
/// actions and the account must still be at the reviewed sequence, so what
/// is signed means what was reviewed.
pub fn recheck(
    client: &Client,
    wallet: &str,
    market_id: &str,
    sequence: u64,
) -> Result<(), String> {
    let view = fresh(client, wallet, market_id)?;
    tradable(&view)?;
    match view.account {
        Some(a) if a.next_sequence == sequence => Ok(()),
        _ => Err(
            "The trading account changed since the review. Nothing was signed; review it again."
                .into(),
        ),
    }
}

/// The open order on one side, the one a new order replaces.
fn open_order(account: &Account, side: &str) -> Option<Order> {
    account
        .orders
        .iter()
        .find(|o| o.side == side && o.remaining != "0")
        .cloned()
}

/// Checks an order against the market as it is now and drafts it. Prices
/// are B3 atoms per base unit and quantities base units, as exact integer
/// strings; they must lie on the market's steps and are never rounded here.
pub fn prepare_order(
    client: &Client,
    chain: &str,
    wallet: &str,
    market_id: &str,
    side: &str,
    steps: &[Point],
    acknowledged: &[String],
) -> Result<OrderDraft, String> {
    let side = side_of(side)?;
    if steps.is_empty() || steps.len() > LADDER_MAX_STEPS {
        return Err(format!("An order has 1 to {LADDER_MAX_STEPS} prices."));
    }
    let view = fresh(client, wallet, market_id)?;
    tradable(&view)?;
    let metadata = view
        .metadata
        .clone()
        .filter(|m| m.decimals.is_some())
        .ok_or("The token's decimals are not verified, so the order cannot be checked.")?;
    let account = view
        .account
        .clone()
        .ok_or("This wallet has no trading account in this market. Deposit first.")?;
    let parsed = check_steps(side, steps, &view.price_tick, &view.quantity_lot)?;
    let replaces = open_order(&account, side);
    let held: u128 = replaces
        .as_ref()
        .and_then(|o| o.reserved.parse().ok())
        .unwrap_or(0);
    let (reserve, free) = if side == "bid" {
        (
            parsed
                .iter()
                .map(|(p, q)| *p as u128 * *q as u128)
                .sum::<u128>(),
            account.b3_available.parse::<u128>().unwrap_or(0),
        )
    } else {
        (
            parsed.iter().map(|(_, q)| *q as u128).sum(),
            account.base_available.parse().unwrap_or(0),
        )
    };
    let available = free + held;
    if reserve > available {
        return Err(if side == "bid" {
            "The order costs more B3 than this trading account has available.".into()
        } else {
            "The order sells more of the token than this trading account has available.".into()
        });
    }
    check_pending(
        client,
        wallet,
        market_id,
        account.next_sequence,
        acknowledged,
    )?;
    Ok(OrderDraft {
        chain: chain.to_owned(),
        wallet: wallet.to_owned(),
        market_id: market_id.to_owned(),
        base_asset_id: view.base_asset_id,
        metadata,
        price_tick: view.price_tick,
        side: side.to_owned(),
        steps: parsed
            .iter()
            .map(|(p, q)| Point {
                price: p.to_string(),
                quantity: q.to_string(),
            })
            .collect(),
        sequence: account.next_sequence,
        reserve: reserve.to_string(),
        available: available.to_string(),
        replaces,
        remote: view.verification.source == "remote_endpoint",
    })
}

/// An order's prices and quantities as exact integers on the market's
/// steps, best price first. Off-grid values are refused, never rounded.
fn check_steps(
    side: &str,
    steps: &[Point],
    tick: &str,
    lot: &str,
) -> Result<Vec<(u64, u64)>, String> {
    let tick = integer(tick, "price step")?;
    let lot = integer(lot, "quantity step")?;
    let mut parsed: Vec<(u64, u64)> = Vec::new();
    for step in steps {
        let price = integer(&step.price, "price")?;
        let quantity = integer(&step.quantity, "quantity")?;
        if price == 0 || quantity == 0 {
            return Err("Every price and quantity must be above zero.".into());
        }
        if tick == 0 || price % tick != 0 {
            return Err("A price is not on the market's price step.".into());
        }
        if lot == 0 || quantity % lot != 0 {
            return Err("A quantity is not on the market's quantity step.".into());
        }
        if parsed.iter().any(|(p, _)| *p == price) {
            return Err("A ladder's prices must all differ.".into());
        }
        parsed.push((price, quantity));
    }
    if side == "bid" {
        parsed.sort_by_key(|s| std::cmp::Reverse(s.0));
    } else {
        parsed.sort_by_key(|s| s.0);
    }
    Ok(parsed)
}

pub fn prepare_cancel(
    client: &Client,
    chain: &str,
    wallet: &str,
    market_id: &str,
    side: &str,
    acknowledged: &[String],
) -> Result<CancelDraft, String> {
    let side = side_of(side)?;
    let view = fresh(client, wallet, market_id)?;
    tradable(&view)?;
    let account = view
        .account
        .clone()
        .ok_or("This wallet has no trading account in this market.")?;
    let order =
        open_order(&account, side).ok_or("There is no open order on that side to cancel.")?;
    check_pending(
        client,
        wallet,
        market_id,
        account.next_sequence,
        acknowledged,
    )?;
    Ok(CancelDraft {
        chain: chain.to_owned(),
        wallet: wallet.to_owned(),
        market_id: market_id.to_owned(),
        base_asset_id: view.base_asset_id,
        metadata: view.metadata.unwrap_or(Metadata {
            decimals: None,
            ticker: String::new(),
            name: String::new(),
            test_only: false,
        }),
        price_tick: view.price_tick,
        side: side.to_owned(),
        sequence: account.next_sequence,
        order,
        remote: view.verification.source == "remote_endpoint",
    })
}

/// The exact RPC call an approved order makes: one price is a limit order,
/// more are a ladder.
pub fn order_call(draft: &OrderDraft) -> Result<(&'static str, Value), String> {
    let steps: Vec<(u64, u64)> = draft
        .steps
        .iter()
        .map(|s| {
            Ok((
                integer(&s.price, "price")?,
                integer(&s.quantity, "quantity")?,
            ))
        })
        .collect::<Result<_, String>>()?;
    Ok(match steps.as_slice() {
        [(price, quantity)] => (
            "submitflowmeshorder",
            json!([
                draft.market_id,
                draft.side,
                price,
                quantity,
                draft.sequence,
                SUBMIT_WAIT_MS
            ]),
        ),
        _ => (
            "submitflowmeshladder",
            json!([
                draft.market_id,
                draft.side,
                steps
                    .iter()
                    .map(|(p, q)| json!({"price": p, "quantity": q}))
                    .collect::<Vec<_>>(),
                draft.sequence,
                SUBMIT_WAIT_MS
            ]),
        ),
    })
}

pub fn cancel_call(draft: &CancelDraft) -> (&'static str, Value) {
    (
        "cancelflowmeshorder",
        json!([draft.market_id, draft.side, draft.sequence, SUBMIT_WAIT_MS]),
    )
}

/// Signs and submits an approved order or cancellation. The node keeps the
/// signed action, so an unknown outcome is never signed again.
pub fn submit(
    client: &Client,
    wallet: &str,
    (method, params): (&str, Value),
) -> Result<Receipt, String> {
    let reply = client
        .call_wallet(wallet, method, params)
        .map_err(|e| e.to_string())?;
    Ok(receipt(&reply))
}

fn receipt(r: &Value) -> Receipt {
    Receipt {
        action_id: text(&r["action_id"]),
        accepted: r["accepted"].as_bool().unwrap_or(false),
        state: text(&r["receipt_state"]),
        stage: text(&r["stage"]),
        reason: text(&r["reason"]),
        outcome: outcome(r),
    }
}

fn outcome(r: &Value) -> Outcome {
    let Some(o) = r.get("outcome").filter(|o| o.is_object()) else {
        return Outcome::default();
    };
    let fill = o.get("fill").filter(|f| f.is_object());
    Outcome {
        status: Some(text(&o["status"])),
        text: text(&o["reason_text"]),
        fill_quantity: fill.map(|f| int(&f["quantity"])),
        fill_price: fill
            .filter(|f| f["cleared"].as_bool() == Some(true))
            .map(|f| int(&f["price"])),
    }
}

/// This wallet's retained requests in a market, newest first.
fn requests(client: &Client, wallet: &str, market_id: &str) -> Result<Vec<Request>, RpcError> {
    let list = client.call_wallet(wallet, "listflowmeshactions", json!([market_id]))?;
    Ok(request_list(&list))
}

fn request_list(list: &Value) -> Vec<Request> {
    let mut out: Vec<Request> = list["actions"]
        .as_array()
        .map(|all| all.iter().map(request).collect())
        .unwrap_or_default();
    out.sort_by_key(|r| std::cmp::Reverse(r.submitted_ms));
    out
}

fn request(a: &Value) -> Request {
    let r = &a["receipt"];
    let points: Vec<Point> = a["canonical_points"]
        .as_array()
        .map(|ps| {
            ps.iter()
                .map(|p| Point {
                    price: int(&p["price"]),
                    quantity: int(&p["quantity"]),
                })
                .collect()
        })
        .unwrap_or_default();
    let side = a["canonical_side"].as_str().map(str::to_owned);
    let kind = match (&side, points.is_empty()) {
        (Some(_), false) => "order",
        (Some(_), true) => "cancel",
        (None, _) => "other",
    };
    Request {
        action_id: text(&a["action_id"]),
        kind: kind.into(),
        side,
        points,
        sequence: a["sequence"].as_u64(),
        submitted_ms: a["initial_submission_ms"].as_u64().unwrap_or(0),
        state: text(&r["receipt_state"]),
        stage: text(&r["stage"]),
        reason: text(&r["reason"]),
        certified: a["previously_certified"].as_bool().unwrap_or(false)
            || r["receipt_state"] == "certified_inclusion",
        outcome: outcome(r),
        settled: false,
        pending: false,
        uncertain: false,
    }
}

fn text(v: &Value) -> String {
    v.as_str().unwrap_or_default().to_owned()
}

/// An exact non-negative integer as a decimal string; the node also writes
/// some as strings already. Anything else (absent, negative, fractional)
/// becomes "0" rather than an inexact value.
fn int(v: &Value) -> String {
    match v {
        Value::Number(n) => n
            .as_u64()
            .map(|n| n.to_string())
            .unwrap_or_else(|| "0".into()),
        Value::String(s) if !s.is_empty() && s.bytes().all(|b| b.is_ascii_digit()) => s.clone(),
        _ => "0".into(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample() -> Value {
        json!({
            "domain": "79", "market_id": "2a".repeat(32), "base_asset_id": "e3".repeat(32),
            "quote_asset": "B3", "quantity_lot_raw": 1, "price_tick_raw": 1, "unchanged": false,
            "verification": {"source": "remote_endpoint", "endpoint": "https://127.0.0.1:19950",
                "certificate_verified": true, "account_state_verified": true,
                "history_endpoint_reported": true, "event_gap": true},
            "base_metadata": {"known": true, "decimals": 2, "ticker": "", "name": "",
                "source": "node-issuance", "test_only": false},
            "snapshot": {"certified": true, "next_microblock_sequence": 17,
                "last_microblock_hash": "99".repeat(32), "anchor_height": 176, "active_seats": 4,
                "quorum_required": 3, "running": true, "paused": false, "chain_reconciling": false,
                "pending_handoff": false, "halt": "none", "error": "", "pending_actions": 0},
            "account": {"account_id": "2e".repeat(32), "next_sequence": 7, "base_available": 9,
                "base_reserved": 0, "b3_available_atoms": 435000000u64, "b3_reserved_atoms": 90000000,
                "curves": [{"side": "bid", "status": "open", "filled_quantity": 0,
                    "remaining_quantity": 3, "reserved_amount": 90000000,
                    "points": [{"price": 30000000, "quantity": 3}, {"price": 30000001, "quantity": 0}]}]},
            "book_levels": {"available": true, "head": "99".repeat(32), "best_bid": 30000000,
                "best_ask": 80000000,
                "bids": {"available": true, "more": false, "levels": [{"bucket": "30000000",
                    "low": 30000000, "high": 30000000, "size": 3, "cumulative": 3,
                    "notional": "90000000", "curves": 1, "own": 3}]},
                "asks": {"available": true, "more": false, "levels": []},
                "standing": {"available": true, "cleared": false, "price": 0, "volume": 0}},
            "clearing_ticks": {"available": true, "gap": false, "ticks": [
                {"sequence": 4, "kind": "execution", "cleared": true, "price": 40000000, "volume": 1},
                {"sequence": 5, "kind": "execution", "cleared": false, "price": 0, "volume": 0}]},
            "history": {"available": true, "entries": [
                {"sequence": 5, "cleared": false, "price": 0, "quantity": 0},
                {"sequence": 4, "cleared": true, "price": 40000000, "quantity": 1,
                    "notional_atoms": 40000000, "fee_atoms": 4000, "account_fills_known": true,
                    "account_bid_fill": 1, "account_ask_fill": 0},
                {"sequence": 3, "cleared": true, "price": 41000000, "quantity": 2,
                    "notional_atoms": 82000000, "fee_atoms": 8200, "account_fills_known": false}]},
            "observed_time": {"available": true, "rows": [
                {"sequence": 3, "mode": "delayed", "applied_here_ms": 1792320600000u64},
                {"sequence": 4, "mode": "live", "observed_ms": 1792320642563u64}]},
            "watch": {"supported": true, "waited": true}
        })
    }

    #[test]
    fn a_market_reply_maps_to_exact_strings() {
        let view = market_view(&sample());
        assert_eq!(view.metadata.as_ref().unwrap().decimals, Some(2));
        assert_eq!(view.status.head, "99".repeat(32));
        assert_eq!(view.status.quorum_required, 3);
        let account = view.account.unwrap();
        assert_eq!(account.b3_available, "435000000");
        assert_eq!(account.orders[0].points[1].price, "30000001");
        let book = view.book.unwrap();
        assert_eq!(book.best_ask.as_deref(), Some("80000000"));
        assert_eq!(book.bids.unwrap().levels[0].own, "3");
        assert!(book.asks.unwrap().levels.is_empty());
        assert!(!book.standing.unwrap().cleared);
        assert_eq!(view.ticks.len(), 2);
        assert!(view.waited);
    }

    #[test]
    fn trades_are_the_cleared_entries_with_known_fills_and_live_times() {
        let trades = market_view(&sample()).trades.unwrap();
        assert_eq!(trades.len(), 2);
        assert_eq!(trades[0].sequence, 4);
        assert_eq!(trades[0].own_bought.as_deref(), Some("1"));
        assert_eq!(trades[0].observed_ms, Some(1792320642563));
        // Unknown fills stay unknown, and a late-applied time is not shown.
        assert_eq!(trades[1].own_bought, None);
        assert_eq!(trades[1].observed_ms, None);
    }

    #[test]
    fn an_unchanged_reply_has_no_heavy_parts() {
        let view = market_view(&json!({
            "market_id": "2a".repeat(32), "unchanged": true,
            "snapshot": {"last_microblock_hash": "99".repeat(32)},
            "verification": {}, "clearing_ticks": {"ticks": []},
            "watch": {"supported": true, "waited": false}
        }));
        assert!(view.unchanged);
        assert!(view.account.is_none() && view.book.is_none() && view.trades.is_none());
        assert!(!view.waited);
        // Unknown provenance is never reported as verified.
        assert!(!view.verification.certificate_verified);
        assert!(view.verification.history_endpoint_reported);
    }

    #[test]
    fn integers_are_never_made_inexact() {
        assert_eq!(int(&json!(18446744073709551615u64)), "18446744073709551615");
        assert_eq!(int(&json!("662200000000000000")), "662200000000000000");
        for bad in [json!(-1), json!(1.5), json!("1e9"), json!(null), json!("")] {
            assert_eq!(int(&bad), "0");
        }
    }

    #[test]
    fn queries_ask_for_levels_ticks_and_a_live_wait_only_when_asked() {
        let fresh = query_options(&Query::default());
        assert_eq!(fresh["levels"]["depth"], BOOK_DEPTH);
        assert!(fresh.get("known_head").is_none() && fresh.get("watch_ms").is_none());
        assert!(fresh["ticks"].get("after_sequence").is_none());
        let live = query_options(&Query {
            known_head: Some("99".repeat(32)),
            watch: true,
            ticks_after: Some(16),
        });
        assert_eq!(live["watch_ms"], WATCH_MS);
        assert_eq!(live["ticks"]["after_sequence"], 16);
    }

    #[test]
    fn ids_are_checked_before_any_call() {
        assert!(valid_id(&"ab".repeat(32)));
        assert!(!valid_id("ab"));
        assert!(!valid_id(&format!("{}\"", "a".repeat(63))));
    }

    fn pt(price: &str, quantity: &str) -> Point {
        Point {
            price: price.into(),
            quantity: quantity.into(),
        }
    }

    #[test]
    fn order_steps_are_exact_on_grid_and_best_first() {
        let bid = check_steps(
            "bid",
            &[pt("100", "5"), pt("300", "1"), pt("200", "2")],
            "100",
            "1",
        )
        .unwrap();
        assert_eq!(bid, vec![(300, 1), (200, 2), (100, 5)]);
        let ask = check_steps("ask", &[pt("300", "1"), pt("100", "5")], "100", "1").unwrap();
        assert_eq!(ask, vec![(100, 5), (300, 1)]);
        // Off the grid, zero, fractional, repeated: refused, never rounded.
        assert!(check_steps("bid", &[pt("150", "1")], "100", "1").is_err());
        assert!(check_steps("bid", &[pt("100", "3")], "100", "2").is_err());
        assert!(check_steps("bid", &[pt("0", "1")], "1", "1").is_err());
        assert!(check_steps("bid", &[pt("1.5", "1")], "1", "1").is_err());
        assert!(check_steps("bid", &[pt("-1", "1")], "1", "1").is_err());
        assert!(check_steps("bid", &[pt("2", "1"), pt("2", "3")], "1", "1").is_err());
        assert!(check_steps("bid", &[pt("99999999999999999999", "1")], "1", "1").is_err());
    }

    fn order(steps: Vec<Point>) -> OrderDraft {
        OrderDraft {
            chain: "regtest".into(),
            wallet: "w".into(),
            market_id: "2a".repeat(32),
            base_asset_id: "e3".repeat(32),
            metadata: Metadata {
                decimals: Some(2),
                ticker: String::new(),
                name: String::new(),
                test_only: false,
            },
            price_tick: "1".into(),
            side: "bid".into(),
            steps,
            sequence: 7,
            reserve: "0".into(),
            available: "0".into(),
            replaces: None,
            remote: true,
        }
    }

    #[test]
    fn one_price_is_a_limit_order_and_more_are_a_ladder() {
        let (method, params) = order_call(&order(vec![pt("30000000", "3")])).unwrap();
        assert_eq!(method, "submitflowmeshorder");
        assert_eq!(
            params,
            json!(["2a".repeat(32), "bid", 30000000u64, 3, 7, SUBMIT_WAIT_MS])
        );
        let (method, params) = order_call(&order(vec![pt("3", "1"), pt("2", "4")])).unwrap();
        assert_eq!(method, "submitflowmeshladder");
        assert_eq!(
            params[2],
            json!([{"price": 3, "quantity": 1}, {"price": 2, "quantity": 4}])
        );
        assert_eq!(params[3], 7);
    }

    fn actions() -> Value {
        json!({"source": "local-retained-outbox", "actions": [
            {"action_id": "a1", "canonical_side": "bid",
                "canonical_points": [{"price": 30000000, "quantity": 3}], "sequence": 6,
                "initial_submission_ms": 1000, "previously_certified": true,
                "receipt": {"receipt_state": "certified_inclusion", "stage": "outcome_verified",
                    "reason": "", "outcome": {"status": "applied", "reason_text": "Applied",
                        "fill": {"side": "bid", "cleared": false, "quantity": 0, "price": 0}}}},
            {"action_id": "a2", "canonical_side": "ask", "canonical_points": [], "sequence": 7,
                "initial_submission_ms": 50000, "previously_certified": false,
                "receipt": {"receipt_state": "queued", "stage": "queued", "reason": "queued"}},
            {"action_id": "a3", "initial_submission_ms": 2000, "previously_certified": false,
                "receipt": {"receipt_state": "unknown", "stage": "unknown", "reason": ""}}
        ]})
    }

    #[test]
    fn requests_are_newest_first_with_verified_outcomes_only() {
        let list = request_list(&actions());
        let ids: Vec<_> = list.iter().map(|r| r.action_id.as_str()).collect();
        assert_eq!(ids, ["a2", "a3", "a1"]);
        assert_eq!(list[0].kind, "cancel");
        assert_eq!(list[1].kind, "other");
        assert_eq!(list[2].kind, "order");
        let a1 = &list[2].outcome;
        assert_eq!(a1.status.as_deref(), Some("applied"));
        // A verified zero fill is a zero; an auction that did not clear has no price.
        assert_eq!(a1.fill_quantity.as_deref(), Some("0"));
        assert_eq!(a1.fill_price, None);
        assert_eq!(list[0].outcome, Outcome::default());
    }

    #[test]
    fn an_unsettled_request_blocks_until_it_settles_or_is_acknowledged() {
        let mut list = request_list(&actions());
        classify(&mut list, 7, 60000, &[]);
        let a2 = list.iter().find(|r| r.action_id == "a2").unwrap();
        assert!(a2.pending && !a2.uncertain, "queued 10 s ago");
        // Deposits and withdrawals never block orders here.
        assert!(!list.iter().find(|r| r.action_id == "a3").unwrap().pending);
        assert!(list.iter().find(|r| r.action_id == "a1").unwrap().settled);
        classify(&mut list, 7, 50000 + PENDING_ACK_MS, &[]);
        let a2 = list.iter().find(|r| r.action_id == "a2").unwrap();
        assert!(a2.pending && a2.uncertain, "queued 30 s");
        classify(&mut list, 7, 50000 + PENDING_ACK_MS, &["a2".into()]);
        assert!(!list.iter().any(|r| r.pending));
        // Once the account moves past its sequence it has settled.
        classify(&mut list, 8, 60000, &[]);
        assert!(!list.iter().any(|r| r.pending));
    }
}
