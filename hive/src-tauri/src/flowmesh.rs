//! The Trade page's reads: FlowMesh markets, their verified book, trades and
//! clearings, and this wallet's trading account. Every FlowMesh call the
//! page makes goes through here, so a protocol change touches this adapter
//! and its tests rather than the page.
//!
//! Amounts stay exact: prices in B3 atoms per base unit, quantities in base
//! units and B3 in atoms, all as integer strings. The page formats them with
//! BigInt and never parses them as floating point.

use serde::Serialize;
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

#[derive(Debug, Clone, PartialEq, Serialize)]
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

fn text(v: &Value) -> String {
    v.as_str().unwrap_or_default().to_owned()
}

/// An exact non-negative integer as a decimal string; the node also writes
/// some as strings already. Anything else (absent, negative, fractional)
/// becomes "0" rather than an inexact value.
fn int(v: &Value) -> String {
    match v {
        Value::Number(n) => n.as_u64().map(|n| n.to_string()).unwrap_or_else(|| "0".into()),
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
}
