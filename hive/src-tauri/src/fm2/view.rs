//! What the Trade page and the approval window are shown, and the drafts
//! the page may ask for. Everything here is derived in the core from the
//! genesis document, the outbox and verified replies; the page supplies only
//! the user's choices and gets back plain values to show.
//!
//! Amounts, prices and counters cross to the page as decimal strings: they
//! are 64-bit integers, and a JavaScript number is exact only to 2^53.

use std::collections::BTreeMap;

use serde::Serialize;

use super::bytes::{id_from_hex, id_hex, H256};
use super::client::{SeatAdmission, SeatState, Session, Submitted, DEFAULT_MAX_DELAY};
use super::outbox::{Certified, Entry, SeenFill};
use super::types::{
    in_atom_range, outcome_code_name, AccountLeaf, Market, Outcome, Payload, SIDE_BUY, SIDE_SELL,
    TIF_GTC, TIF_IOC, TIF_POST_ONLY,
};

/// Outcome codes from 64 up are rejections (core/codes.h).
const FIRST_REJECTION: u16 = 64;
const CODE_RESTING: u16 = 2;
const CODE_PARTIAL_RESTING: u16 = 4;

// ------------------------------------------------------------------ network

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct SeatInfo {
    pub seat: u32,
    pub address: String,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct AssetInfo {
    pub name: String,
    pub id: String,
    /// Native B3 (the all-zero asset id): its atoms are the chain's base
    /// units, 10^9 to one B3. Every other asset is shown in atoms, because
    /// the genesis document names no decimals for it.
    pub native: bool,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct MarketInfo {
    pub name: String,
    pub id: String,
    pub base: AssetInfo,
    pub quote: AssetInfo,
    pub base_atoms_per_lot: String,
    pub quote_atoms_per_tick: String,
}

/// The connected network, from the genesis document alone.
#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct Info {
    pub chain: &'static str,
    pub profile: &'static str,
    pub domain: String,
    pub seat_set: String,
    pub quorum: usize,
    pub seats: Vec<SeatInfo>,
    pub assets: Vec<AssetInfo>,
    pub markets: Vec<MarketInfo>,
    /// The account of this network's key in B3 Hive, once one was created.
    pub account: Option<String>,
    pub fee_ppm: u32,
    pub max_open_orders: u32,
    /// How many slots an action stays includable after it is signed.
    pub window_slots: u16,
    /// Bytes of a half-written last record the outbox dropped when opened.
    pub recovered_bytes: u64,
}

fn asset_info(session: &Session, id: &H256) -> AssetInfo {
    AssetInfo {
        name: session
            .genesis
            .asset_name(id)
            .map_or_else(|| id_hex(id), str::to_owned),
        id: id_hex(id),
        native: *id == [0u8; 32],
    }
}

fn market_info(session: &Session, name: String, market: &Market) -> MarketInfo {
    MarketInfo {
        name,
        id: id_hex(&market.id()),
        base: asset_info(session, &market.base),
        quote: asset_info(session, &market.quote),
        base_atoms_per_lot: market.base_atoms_per_lot.to_string(),
        quote_atoms_per_tick: market.quote_atoms_per_tick.to_string(),
    }
}

pub fn info(session: &Session, recovered_bytes: u64) -> Info {
    Info {
        chain: session.trust.profile.chain,
        profile: session.trust.profile.profile_id,
        domain: id_hex(&session.trust.domain),
        seat_set: id_hex(session.trust.seats.root()),
        quorum: session.trust.seats.quorum(),
        seats: session
            .seats
            .iter()
            .map(|seat| SeatInfo {
                seat: seat.seat,
                address: std::net::SocketAddr::new(seat.address, seat.port).to_string(),
            })
            .collect(),
        assets: session
            .genesis
            .assets
            .iter()
            .map(|asset| asset_info(session, &asset.id))
            .collect(),
        markets: session
            .genesis
            .markets
            .iter()
            .map(|market| market_info(session, market.name(), &market.descriptor))
            .collect(),
        account: session.account().as_ref().map(id_hex),
        fee_ppm: session.trust.profile.fee_ppm,
        max_open_orders: session.trust.profile.max_open_orders_per_account,
        window_slots: DEFAULT_MAX_DELAY,
        recovered_bytes,
    }
}

// ------------------------------------------------------------------- intent

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct Amount {
    pub asset: AssetInfo,
    pub atoms: String,
}

/// What an action asks for, in the terms of the genesis document.
// A few per page view; boxing the common variant would buy nothing.
#[allow(clippy::large_enum_variant)]
#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(tag = "action", rename_all = "snake_case")]
pub enum Intent {
    Place {
        market: MarketInfo,
        side: &'static str,
        tif: &'static str,
        price_ticks: String,
        lots: String,
        /// lots x the market's lot size.
        base: Amount,
        /// lots x price x the market's tick: the most a buy pays, the least
        /// a sell receives before the fee.
        quote: Amount,
    },
    Cancel {
        order: String,
        /// The order as this outbox signed it, when it did.
        placed: Option<Box<Intent>>,
    },
    CancelAll,
    Transfer {
        to: String,
        amount: Amount,
    },
    /// An action kind this version of B3 Hive does not show.
    Other {
        kind: u8,
    },
}

fn side_name(side: u8) -> &'static str {
    match side {
        SIDE_BUY => "buy",
        SIDE_SELL => "sell",
        _ => "?",
    }
}

fn tif_name(tif: u8) -> &'static str {
    match tif {
        TIF_GTC => "gtc",
        TIF_IOC => "ioc",
        TIF_POST_ONLY => "post_only",
        _ => "?",
    }
}

fn place_intent(session: &Session, payload: &Payload) -> Option<Intent> {
    let Payload::Place {
        market,
        side,
        tif,
        price_ticks,
        lots,
    } = payload
    else {
        return None;
    };
    let listed = session.genesis.market(market)?;
    let descriptor = &listed.descriptor;
    Some(Intent::Place {
        market: market_info(session, listed.name(), descriptor),
        side: side_name(*side),
        tif: tif_name(*tif),
        price_ticks: price_ticks.to_string(),
        lots: lots.to_string(),
        base: Amount {
            asset: asset_info(session, &descriptor.base),
            atoms: descriptor.base_atoms(*lots)?.to_string(),
        },
        quote: Amount {
            asset: asset_info(session, &descriptor.quote),
            atoms: descriptor.quote_atoms(*lots, *price_ticks)?.to_string(),
        },
    })
}

/// `payload` in the page's terms. `entries` resolves the order a
/// cancellation names.
pub fn intent(session: &Session, payload: &Payload, entries: &[Entry]) -> Intent {
    match payload {
        Payload::Place { .. } => place_intent(session, payload).unwrap_or(Intent::Other {
            kind: payload.kind(),
        }),
        Payload::Cancel { order } => Intent::Cancel {
            order: id_hex(order),
            placed: entries
                .iter()
                .find(|entry| entry.action.id() == *order)
                .and_then(payload_of)
                .and_then(|placed| place_intent(session, &placed))
                .map(Box::new),
        },
        Payload::CancelAll => Intent::CancelAll,
        Payload::Transfer { to, asset, amount } => Intent::Transfer {
            to: id_hex(to),
            amount: Amount {
                asset: asset_info(session, asset),
                atoms: amount.to_string(),
            },
        },
    }
}

fn payload_of(entry: &Entry) -> Option<Payload> {
    Payload::decode(entry.action.kind, &entry.action.payload)
        .ok()
        .flatten()
}

// ------------------------------------------------------------------- drafts

/// An action waiting in the approval window. The window sees what it asks
/// for; the payload that will be signed stays in the core.
#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct ActionDraft {
    pub chain: &'static str,
    pub domain: String,
    pub account: String,
    pub intent: Intent,
    pub window_slots: u16,
    pub seats: usize,
    pub fee_ppm: u32,
    #[serde(skip)]
    pub payload: Payload,
    /// The network and account the draft was made for; approving it under
    /// another connection is refused.
    #[serde(skip)]
    pub bound: (H256, H256),
}

/// A whole number written with digits only, as the page sends them.
fn number(text: &str, what: &str) -> Result<u64, String> {
    let digits = !text.is_empty() && text.len() <= 20 && text.bytes().all(|b| b.is_ascii_digit());
    digits
        .then(|| text.parse::<u64>().ok())
        .flatten()
        .ok_or_else(|| format!("{what} is not a whole number."))
}

fn id(text: &str, what: &str) -> Result<H256, String> {
    id_from_hex(text).ok_or_else(|| format!("{what} is not 64 hexadecimal characters."))
}

fn draft(session: &Session, payload: Payload) -> Result<ActionDraft, String> {
    let account = session
        .account()
        .ok_or("This network has no account in B3 Hive yet. Create its key first.")?;
    // What cannot be encoded cannot be signed; say so before the window opens.
    payload.encode().map_err(String::from)?;
    Ok(ActionDraft {
        chain: session.trust.profile.chain,
        domain: id_hex(&session.trust.domain),
        account: id_hex(&account),
        intent: intent(session, &payload, &session.entries()),
        window_slots: DEFAULT_MAX_DELAY,
        seats: session.seats.len(),
        fee_ppm: session.trust.profile.fee_ppm,
        payload,
        bound: (session.trust.domain, account),
    })
}

pub fn draft_place(
    session: &Session,
    market: &str,
    side: &str,
    tif: &str,
    price_ticks: &str,
    lots: &str,
) -> Result<ActionDraft, String> {
    let market = id(market, "The market")?;
    let listed = session
        .genesis
        .market(&market)
        .ok_or("This market is not in the genesis document.")?;
    let side = match side {
        "buy" => SIDE_BUY,
        "sell" => SIDE_SELL,
        _ => return Err("Choose buy or sell.".into()),
    };
    let tif = match tif {
        "gtc" => TIF_GTC,
        "ioc" => TIF_IOC,
        "post_only" => TIF_POST_ONLY,
        _ => return Err("Unknown order type.".into()),
    };
    let price_ticks = number(price_ticks, "The price")?;
    let lots = number(lots, "The amount")?;
    if price_ticks == 0 {
        return Err("The price must be at least one tick.".into());
    }
    if lots == 0 {
        return Err("The amount must be at least one lot.".into());
    }
    if listed.descriptor.base_atoms(lots).is_none()
        || listed.descriptor.quote_atoms(lots, price_ticks).is_none()
    {
        return Err("This order is larger than any amount FlowMesh can hold.".into());
    }
    draft(
        session,
        Payload::Place {
            market,
            side,
            tif,
            price_ticks,
            lots,
        },
    )
}

/// Cancelling is offered for orders this outbox placed; another account's
/// order could not be cancelled with this key anyway.
pub fn draft_cancel(session: &Session, order: &str) -> Result<ActionDraft, String> {
    let order = id(order, "The order")?;
    let own = session.entries().iter().any(|entry| {
        entry.action.id() == order && matches!(payload_of(entry), Some(Payload::Place { .. }))
    });
    if !own {
        return Err("This is not an order placed from B3 Hive with this account.".into());
    }
    draft(session, Payload::Cancel { order })
}

pub fn draft_cancel_all(session: &Session) -> Result<ActionDraft, String> {
    draft(session, Payload::CancelAll)
}

pub fn draft_transfer(
    session: &Session,
    to: &str,
    asset: &str,
    amount: &str,
) -> Result<ActionDraft, String> {
    let to = id(to.trim(), "The recipient account")?;
    let asset = id(asset, "The asset")?;
    if session.genesis.asset_name(&asset).is_none() {
        return Err("This asset is not in the genesis document.".into());
    }
    if session.account() == Some(to) {
        return Err("The recipient is this account itself.".into());
    }
    let amount = i64::try_from(number(amount, "The amount")?)
        .ok()
        .filter(|atoms| *atoms > 0 && in_atom_range(*atoms))
        .ok_or("The amount must be more than zero and within what FlowMesh can hold.")?;
    draft(session, Payload::Transfer { to, asset, amount })
}

/// What approving a draft did: the action is signed and stored, and this is
/// what each seat's admission said. Admitted is not included.
#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct Sent {
    pub label: String,
    pub action_id: String,
    pub nonce: String,
    pub ref_slot: String,
    pub window_slots: u16,
    pub admissions: Vec<Admission>,
    /// How many seats hold the action now.
    pub accepted: usize,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct Admission {
    pub seat: u32,
    pub code: String,
    pub reason: String,
    pub accepted: bool,
}

pub fn admissions(list: &[SeatAdmission]) -> Vec<Admission> {
    list.iter()
        .map(|a| Admission {
            seat: a.seat,
            code: a.code.clone(),
            reason: a.reason.clone(),
            accepted: a.accepted(),
        })
        .collect()
}

pub fn sent(submitted: &Submitted) -> Sent {
    Sent {
        label: submitted.label.clone(),
        action_id: id_hex(&submitted.action_id),
        nonce: submitted.nonce.to_string(),
        ref_slot: submitted.ref_slot.to_string(),
        window_slots: submitted.max_delay,
        admissions: admissions(&submitted.admissions),
        accepted: submitted.admissions.iter().filter(|a| a.accepted()).count(),
    }
}

// ----------------------------------------------------------------- activity

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct FillView {
    pub market: String,
    /// This account's side of the fill.
    pub side: &'static str,
    pub price_ticks: String,
    pub lots: String,
    pub quote: Amount,
    /// The fee this account paid as the seller, in the quote asset.
    pub fee_atoms: Option<String>,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct DeltaView {
    pub asset: AssetInfo,
    pub available: String,
    pub reserved: String,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(tag = "state", rename_all = "snake_case")]
pub enum RowState {
    /// No certified end yet; `note` is what the seats last said.
    Open { note: String },
    /// Certified: a quorum of the seats signed the slot that holds it.
    Included {
        height: String,
        index: u32,
        code: u16,
        code_name: String,
        /// False for a certified rejection.
        accepted: bool,
        remaining_lots: String,
        fills: Vec<FillView>,
        /// This account's balance changes in the outcome.
        deltas: Vec<DeltaView>,
    },
    /// Certified: no slot can include it any more. Nothing happened.
    Expired { height: String },
}

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct ActivityRow {
    pub label: String,
    /// Unix seconds when it was signed, by this computer's clock.
    pub created: u64,
    pub action_id: String,
    pub nonce: String,
    pub ref_slot: String,
    pub window_slots: u16,
    pub intent: Intent,
    #[serde(flatten)]
    pub state: RowState,
}

fn fill_views(session: &Session, outcome: &Outcome) -> Vec<FillView> {
    outcome
        .fills
        .iter()
        .map(|fill| {
            // The outcome is this account's own action, so it is the taker.
            let market = session.genesis.market(&fill.market);
            FillView {
                market: market.map_or_else(|| id_hex(&fill.market), |m| m.name()),
                side: side_name(fill.taker_side),
                price_ticks: fill.price_ticks.to_string(),
                lots: fill.lots.to_string(),
                quote: Amount {
                    asset: market.map_or_else(
                        || asset_info(session, &[0xff; 32]),
                        |m| asset_info(session, &m.descriptor.quote),
                    ),
                    atoms: fill.quote_atoms.to_string(),
                },
                fee_atoms: (fill.taker_side == SIDE_SELL).then(|| fill.seller_fee.to_string()),
            }
        })
        .collect()
}

fn row_state(session: &Session, entry: &Entry, notes: &BTreeMap<String, String>) -> RowState {
    match &entry.result {
        None => RowState::Open {
            note: notes.get(&entry.label).cloned().unwrap_or_default(),
        },
        Some(Certified::Expired { height }) => RowState::Expired {
            height: height.to_string(),
        },
        Some(Certified::Included {
            height,
            index,
            outcome,
        }) => RowState::Included {
            height: height.to_string(),
            index: *index,
            code: outcome.code,
            code_name: outcome_code_name(outcome.code)
                .map_or_else(|| format!("code {}", outcome.code), str::to_owned),
            accepted: outcome.code < FIRST_REJECTION,
            remaining_lots: outcome.remaining_lots.to_string(),
            fills: fill_views(session, outcome),
            deltas: outcome
                .deltas
                .iter()
                .filter(|delta| delta.account == outcome.account)
                .map(|delta| DeltaView {
                    asset: asset_info(session, &delta.asset),
                    available: delta.available_delta.to_string(),
                    reserved: delta.reserved_delta.to_string(),
                })
                .collect(),
        },
    }
}

/// The outbox, newest first.
pub fn activity(
    session: &Session,
    entries: &[Entry],
    notes: &BTreeMap<String, String>,
) -> Vec<ActivityRow> {
    entries
        .iter()
        .rev()
        .map(|entry| ActivityRow {
            label: entry.label.clone(),
            created: entry.created,
            action_id: id_hex(&entry.action.id()),
            nonce: entry.action.nonce.to_string(),
            ref_slot: entry.action.ref_slot.to_string(),
            window_slots: entry.action.max_delay,
            intent: payload_of(entry).map_or(
                Intent::Other {
                    kind: entry.action.kind,
                },
                |payload| intent(session, &payload, entries),
            ),
            state: row_state(session, entry, notes),
        })
        .collect()
}

// -------------------------------------------------------------- open orders

/// An order of this account that should still be resting, rebuilt from the
/// outbox: its certified outcome said it rested, no certified cancellation
/// followed, and the fills a seat's trade stream listed for it since were
/// taken off. FlowMesh V2 has no query for an account's orders, so fills
/// that happened while B3 Hive was not watching are not known here.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct OpenOrder {
    pub order: H256,
    pub label: String,
    pub market: H256,
    pub side: u8,
    pub price_ticks: u64,
    pub placed_lots: u64,
    /// Certified at the slot that included the order.
    pub rested_lots: u64,
    /// Lots the trade stream listed as filled since. A seat's claim.
    pub seen_filled_lots: u64,
    pub remaining_lots: u64,
    pub height: u64,
    /// A cancellation of it is signed and has no certified end yet.
    pub cancelling: bool,
}

pub fn open_orders(entries: &[Entry], fills: &[SeenFill]) -> Vec<OpenOrder> {
    // Certified cancellations: of one order, or of everything before a point.
    let mut cancelled: Vec<H256> = Vec::new();
    let mut cancelling: Vec<H256> = Vec::new();
    let mut swept_before: Option<(u64, u32)> = None;
    let mut sweeping = false;
    for entry in entries {
        match (payload_of(entry), &entry.result) {
            (Some(Payload::Cancel { order }), Some(Certified::Included { outcome, .. }))
                if outcome.code < FIRST_REJECTION =>
            {
                cancelled.push(order)
            }
            (Some(Payload::Cancel { order }), None) => cancelling.push(order),
            (
                Some(Payload::CancelAll),
                Some(Certified::Included {
                    height,
                    index,
                    outcome,
                }),
            ) if outcome.code < FIRST_REJECTION => {
                swept_before = swept_before.max(Some((*height, *index)));
            }
            (Some(Payload::CancelAll), None) => sweeping = true,
            _ => {}
        }
    }
    let mut orders = Vec::new();
    for entry in entries {
        let (
            Some(Payload::Place {
                market,
                side,
                price_ticks,
                lots,
                ..
            }),
            Some(Certified::Included {
                height,
                index,
                outcome,
            }),
        ) = (payload_of(entry), &entry.result)
        else {
            continue;
        };
        if !matches!(outcome.code, CODE_RESTING | CODE_PARTIAL_RESTING)
            || outcome.remaining_lots == 0
        {
            continue;
        }
        let order = entry.action.id();
        if cancelled.contains(&order) || swept_before.is_some_and(|at| (*height, *index) < at) {
            continue;
        }
        let seen: u64 = fills
            .iter()
            .filter(|fill| fill.order == order)
            .fold(0u64, |sum, fill| sum.saturating_add(fill.lots));
        let remaining = outcome.remaining_lots.saturating_sub(seen);
        if remaining == 0 {
            continue;
        }
        orders.push(OpenOrder {
            order,
            label: entry.label.clone(),
            market,
            side,
            price_ticks,
            placed_lots: lots,
            rested_lots: outcome.remaining_lots,
            seen_filled_lots: seen,
            remaining_lots: remaining,
            height: *height,
            cancelling: sweeping || cancelling.contains(&order),
        });
    }
    orders
}

/// Whether the rebuilt orders fit the certified account leaf, and if not,
/// why. The leaf carries a count of open orders and the reserved balance per
/// asset, not the orders themselves:
/// - the count must be the number of rebuilt orders;
/// - an asset that is only ever a base asset is reserved by sells alone,
///   lot for lot, so its reserved balance must equal the rebuilt sells;
/// - a quote asset is reserved by buys at no less than their limit price, so
///   its reserved balance must cover the rebuilt buys.
pub fn orders_against_leaf(
    session: &Session,
    orders: &[OpenOrder],
    leaf: Option<&AccountLeaf>,
) -> Result<(), String> {
    let count = leaf.map_or(0, |leaf| leaf.open_orders);
    if count as usize != orders.len() {
        return Err(format!(
            "The certified account has {count} open order(s); B3 Hive's record gives {}.",
            orders.len()
        ));
    }
    let mut expected: BTreeMap<H256, i128> = BTreeMap::new();
    for order in orders {
        let Some(market) = session.genesis.market(&order.market) else {
            continue;
        };
        let market = &market.descriptor;
        let (asset, atoms) = if order.side == SIDE_SELL {
            (market.base, market.base_atoms(order.remaining_lots))
        } else {
            (
                market.quote,
                market.quote_atoms(order.remaining_lots, order.price_ticks),
            )
        };
        *expected.entry(asset).or_default() += i128::from(atoms.unwrap_or(0));
    }
    for asset in &session.genesis.assets {
        let reserved = leaf
            .and_then(|leaf| leaf.balances.iter().find(|b| b.asset == asset.id))
            .map_or(0, |balance| i128::from(balance.reserved));
        let rebuilt = expected.get(&asset.id).copied().unwrap_or(0);
        let is_quote = session
            .genesis
            .markets
            .iter()
            .any(|market| market.descriptor.quote == asset.id);
        let fits = if is_quote {
            reserved >= rebuilt
        } else {
            reserved == rebuilt
        };
        if !fits {
            return Err(format!(
                "The certified account reserves {reserved} atoms of {}; B3 Hive's record of the open orders gives {rebuilt}.",
                asset.name
            ));
        }
    }
    Ok(())
}

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct OrderView {
    pub order: String,
    pub label: String,
    pub market: String,
    pub market_id: String,
    pub side: &'static str,
    pub price_ticks: String,
    pub placed_lots: String,
    pub rested_lots: String,
    pub seen_filled_lots: String,
    pub remaining_lots: String,
    pub height: String,
    pub cancelling: bool,
}

fn order_view(session: &Session, order: &OpenOrder) -> OrderView {
    OrderView {
        order: id_hex(&order.order),
        label: order.label.clone(),
        market: session
            .genesis
            .market(&order.market)
            .map_or_else(|| id_hex(&order.market), |market| market.name()),
        market_id: id_hex(&order.market),
        side: side_name(order.side),
        price_ticks: order.price_ticks.to_string(),
        placed_lots: order.placed_lots.to_string(),
        rested_lots: order.rested_lots.to_string(),
        seen_filled_lots: order.seen_filled_lots.to_string(),
        remaining_lots: order.remaining_lots.to_string(),
        height: order.height.to_string(),
        cancelling: order.cancelling,
    }
}

// ------------------------------------------------------------------ account

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct SeatAnswer {
    pub seat: u32,
    /// The seat's head, once its certificate verified.
    pub height: Option<String>,
    /// The seat proved the account's leaf under that head.
    pub proved: bool,
    pub error: Option<String>,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct BalanceView {
    pub asset: AssetInfo,
    pub available: String,
    pub reserved: String,
}

/// The account as the seats proved it, with B3 Hive's own record beside it.
#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct AccountView {
    pub account: Option<String>,
    pub seats: Vec<SeatAnswer>,
    /// The head of the proved leaf that is shown: the highest one.
    pub height: Option<String>,
    /// True once a seat proved the leaf. False with a height means the seats
    /// say the account does not exist, which this version cannot prove.
    pub proved: bool,
    pub balances: Vec<BalanceView>,
    pub open_orders: Option<u32>,
    pub orders: Vec<OrderView>,
    /// Why the rebuilt orders do not fit the certified leaf; None when they
    /// do, or when no seat answered.
    pub orders_mismatch: Option<String>,
    pub activity: Vec<ActivityRow>,
}

pub fn account(
    session: &Session,
    states: &[SeatState],
    notes: &BTreeMap<String, String>,
) -> AccountView {
    let entries = session.entries();
    let orders = open_orders(&entries, &session.seen_fills());
    let best = states
        .iter()
        .filter(|state| state.leaf.is_some())
        .max_by_key(|state| state.height);
    let leaf = best.and_then(|state| state.leaf.as_ref());
    let answered = states.iter().filter_map(|state| state.height).max();
    // Every genesis asset, then whatever else the leaf holds.
    let mut balances: Vec<BalanceView> = session
        .genesis
        .assets
        .iter()
        .map(|asset| {
            let held = leaf.and_then(|leaf| leaf.balances.iter().find(|b| b.asset == asset.id));
            BalanceView {
                asset: asset_info(session, &asset.id),
                available: held.map_or(0, |b| b.available).to_string(),
                reserved: held.map_or(0, |b| b.reserved).to_string(),
            }
        })
        .collect();
    for held in leaf.map_or(&[][..], |leaf| &leaf.balances) {
        if session.genesis.asset_name(&held.asset).is_none() {
            balances.push(BalanceView {
                asset: asset_info(session, &held.asset),
                available: held.available.to_string(),
                reserved: held.reserved.to_string(),
            });
        }
    }
    AccountView {
        account: session.account().as_ref().map(id_hex),
        seats: states
            .iter()
            .map(|state| SeatAnswer {
                seat: state.seat,
                height: state.height.map(|height| height.to_string()),
                proved: state.leaf.is_some(),
                error: state.error.clone(),
            })
            .collect(),
        height: best
            .and_then(|state| state.height)
            .or(answered)
            .map(|height| height.to_string()),
        proved: leaf.is_some(),
        open_orders: answered.map(|_| leaf.map_or(0, |leaf| leaf.open_orders)),
        orders_mismatch: answered.and_then(|_| orders_against_leaf(session, &orders, leaf).err()),
        orders: orders
            .iter()
            .map(|order| order_view(session, order))
            .collect(),
        balances,
        activity: activity(session, &entries, notes),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::fm2::bytes::unhex;
    use crate::fm2::crypto::SecretKey;
    use crate::fm2::types::{Action, Profile};

    const GENESIS: &str = include_str!("../../tests/fixtures/fm2/genesis.json");
    const PINS: &str = include_str!("../../tests/fixtures/fm2/pins.json");
    const RESTING: &str = include_str!("../../tests/fixtures/fm2/status_resting.json");
    const ALICE_ACTION: &str = include_str!("../../tests/fixtures/fm2/alice_action.hex");

    struct Folder(std::path::PathBuf);

    impl Drop for Folder {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    fn session(name: &str) -> (Session, Folder) {
        let dir = std::env::temp_dir().join(format!("b3hive-view-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let session = Session::open(GENESIS, PINS, &dir).unwrap();
        (session, Folder(dir))
    }

    /// Alice's real resting sell (10 lots at 100 ticks) with its certified outcome.
    fn resting() -> Entry {
        let profile = Profile::dev_regtest();
        let bytes = unhex(ALICE_ACTION.trim()).unwrap();
        let reply: serde_json::Value = serde_json::from_str(RESTING).unwrap();
        let encoded = unhex(reply["included"]["outcome"]["encoded"].as_str().unwrap()).unwrap();
        Entry {
            label: "PLACE-1".into(),
            created: 0,
            action: Action::decode(&bytes, &profile).unwrap(),
            bytes,
            result: Some(Certified::Included {
                height: 407,
                index: 0,
                outcome: Outcome::decode(&encoded, &profile).unwrap(),
            }),
        }
    }

    /// An entry of `payload` with a certified outcome of `code`, or none.
    fn entry(label: &str, nonce: u64, payload: Payload, end: Option<(u64, u16)>) -> Entry {
        let profile = Profile::dev_regtest();
        let (_, key) = SecretKey::generate();
        let mut action = Action {
            domain: [1; 32],
            signer: key.public(),
            nonce,
            ref_slot: 400,
            max_delay: 64,
            kind: payload.kind(),
            payload: payload.encode().unwrap(),
            sig: [0; 64],
        };
        action.sig = key.sign(&action.id());
        let result = end.map(|(height, code)| {
            // source | subject | account | nonce | code | remaining | no fills | no deltas
            let mut bytes = vec![2u8];
            bytes.extend_from_slice(&action.id());
            bytes.extend_from_slice(&action.account());
            bytes.extend_from_slice(&nonce.to_le_bytes());
            bytes.extend_from_slice(&code.to_le_bytes());
            bytes.extend_from_slice(&0u64.to_le_bytes());
            bytes.extend_from_slice(&[0, 0]);
            Certified::Included {
                height,
                index: 0,
                outcome: Outcome::decode(&bytes, &profile).unwrap(),
            }
        });
        Entry {
            label: label.into(),
            created: 0,
            bytes: action.encode(),
            action,
            result,
        }
    }

    fn seen(order: H256, slot: u64, lots: u64) -> SeenFill {
        SeenFill {
            order,
            taker: [9; 32],
            slot,
            price_ticks: 100,
            lots,
        }
    }

    #[test]
    fn a_resting_order_stays_open_until_filled_or_cancelled() {
        let order = resting();
        let id = order.action.id();
        let open = open_orders(std::slice::from_ref(&order), &[]);
        assert_eq!(open.len(), 1);
        assert_eq!(
            (open[0].side, open[0].price_ticks, open[0].remaining_lots),
            (SIDE_SELL, 100, 10)
        );
        assert!(!open[0].cancelling);

        // Fills the trade stream listed come off; other orders' fills do not.
        let fills = [seen(id, 408, 4), seen([7; 32], 408, 3)];
        let open = open_orders(std::slice::from_ref(&order), &fills);
        assert_eq!((open[0].seen_filled_lots, open[0].remaining_lots), (4, 6));
        let fills = [seen(id, 408, 4), seen(id, 409, 6)];
        assert!(open_orders(std::slice::from_ref(&order), &fills).is_empty());

        // A signed cancellation marks it; a certified one closes it; a
        // certified rejection of the cancellation changes nothing.
        let cancel = Payload::Cancel { order: id };
        let waiting = [order.clone(), entry("CANCEL-2", 2, cancel.clone(), None)];
        assert!(open_orders(&waiting, &[])[0].cancelling);
        let done = [
            order.clone(),
            entry("CANCEL-2", 2, cancel.clone(), Some((410, 6))),
        ];
        assert!(open_orders(&done, &[]).is_empty());
        let refused = [order.clone(), entry("CANCEL-2", 2, cancel, Some((410, 66)))];
        let open = open_orders(&refused, &[]);
        assert_eq!(open.len(), 1);
        assert!(!open[0].cancelling);
    }

    #[test]
    fn cancel_all_closes_what_rested_before_it() {
        let order = resting();
        let before = [
            entry("CANCEL_ALL-0", 9, Payload::CancelAll, Some((300, 6))),
            order.clone(),
        ];
        assert_eq!(open_orders(&before, &[]).len(), 1, "an earlier sweep");
        let after = [
            order.clone(),
            entry("CANCEL_ALL-2", 2, Payload::CancelAll, Some((500, 6))),
        ];
        assert!(open_orders(&after, &[]).is_empty());
        let waiting = [order, entry("CANCEL_ALL-2", 2, Payload::CancelAll, None)];
        assert!(open_orders(&waiting, &[])[0].cancelling);
    }

    #[test]
    fn orders_that_never_rested_are_not_open() {
        let place = Payload::Place {
            market: [3; 32],
            side: SIDE_BUY,
            tif: TIF_IOC,
            price_ticks: 5,
            lots: 2,
        };
        let entries = [
            entry("PLACE-1", 1, place.clone(), None),
            entry("PLACE-2", 2, place.clone(), Some((10, 3))),
            entry("PLACE-3", 3, place.clone(), Some((10, 64))),
            // RESTING with nothing left cannot be; it is not shown either.
            entry("PLACE-4", 4, place, Some((10, 2))),
        ];
        assert!(open_orders(&entries, &[]).is_empty());
    }

    #[test]
    fn the_rebuilt_orders_are_checked_against_the_certified_leaf() {
        let (session, _folder) = session("leaf");
        let reply: serde_json::Value =
            serde_json::from_str(include_str!("../../tests/fixtures/fm2/state_alice.json"))
                .unwrap();
        let state = crate::fm2::reply::parse_state(&reply, &session.trust.profile).unwrap();
        // The leaf of the fixture: one open order, 6 lots of B3 still reserved.
        let leaf = state.witness.unwrap().leaf;
        let order = resting();
        let id = order.action.id();

        let exact = open_orders(std::slice::from_ref(&order), &[seen(id, 408, 4)]);
        assert_eq!(orders_against_leaf(&session, &exact, Some(&leaf)), Ok(()));
        // A fill that happened while nothing was watching: the record still
        // says 10 lots, the leaf reserves 6.
        let stale = open_orders(std::slice::from_ref(&order), &[]);
        let why = orders_against_leaf(&session, &stale, Some(&leaf)).unwrap_err();
        assert!(why.contains("6000000") && why.contains("10000000"), "{why}");
        // An order the record does not know, or one it wrongly keeps.
        let why = orders_against_leaf(&session, &[], Some(&leaf)).unwrap_err();
        assert!(why.contains("1 open order"), "{why}");
        assert!(orders_against_leaf(&session, &exact, None).is_err());
        assert_eq!(orders_against_leaf(&session, &[], None), Ok(()));
    }

    #[test]
    fn drafts_are_checked_before_the_approval_window_opens() {
        let (session, _folder) = session("drafts");
        let market = id_hex(&session.genesis.markets[0].descriptor.id());
        // No key yet: nothing can be drafted.
        let why = draft_place(&session, &market, "buy", "gtc", "100", "2").unwrap_err();
        assert!(why.contains("no account"), "{why}");
        let account = session.create_key().unwrap();

        let placed = draft_place(&session, &market, "buy", "post_only", "100", "2").unwrap();
        assert_eq!(placed.bound, (session.trust.domain, account));
        let Intent::Place {
            market: shown,
            side,
            tif,
            base,
            quote,
            ..
        } = &placed.intent
        else {
            panic!("a place intent")
        };
        assert_eq!(
            (shown.name.as_str(), *side, *tif),
            ("B3/rUSD", "buy", "post_only")
        );
        // 2 lots of 1,000,000 atoms of B3, for 2 x 100 ticks of 1 atom of rUSD.
        assert_eq!((base.atoms.as_str(), base.asset.native), ("2000000", true));
        assert_eq!(
            (quote.atoms.as_str(), quote.asset.name.as_str()),
            ("200", "rUSD")
        );
        // The window is shown the intent; the payload stays in the core.
        let shown = serde_json::to_value(&placed).unwrap();
        assert_eq!(shown["intent"]["action"], "place");
        assert!(shown.get("payload").is_none() && shown.get("bound").is_none());

        for (side, tif, price, lots) in [
            ("hold", "gtc", "100", "2"),
            ("buy", "market", "100", "2"),
            ("buy", "gtc", "0", "2"),
            ("buy", "gtc", "100", "0"),
            ("buy", "gtc", "1.5", "2"),
            ("buy", "gtc", "-1", "2"),
            ("buy", "gtc", "100", ""),
            ("buy", "gtc", "100", "99999999999999999999"),
            ("buy", "gtc", "18446744073709551615", "18446744073709551615"),
        ] {
            assert!(
                draft_place(&session, &market, side, tif, price, lots).is_err(),
                "{side} {tif} {price} {lots}"
            );
        }
        assert!(draft_place(&session, &id_hex(&[5; 32]), "buy", "gtc", "1", "1").is_err());
        assert!(draft_place(&session, "abc", "buy", "gtc", "1", "1").is_err());

        // Only orders this outbox placed can be cancelled from here.
        assert!(draft_cancel(&session, &id_hex(&[5; 32])).is_err());
        assert!(matches!(
            draft_cancel_all(&session).unwrap().intent,
            Intent::CancelAll
        ));

        let rusd = id_hex(&session.genesis.assets[2].id);
        let to = id_hex(&[8; 32]);
        let transfer = draft_transfer(&session, &to, &rusd, "25").unwrap();
        assert!(matches!(
            transfer.payload,
            Payload::Transfer { amount: 25, .. }
        ));
        assert!(draft_transfer(&session, &id_hex(&account), &rusd, "25").is_err());
        assert!(draft_transfer(&session, &to, &id_hex(&[4; 32]), "25").is_err());
        assert!(draft_transfer(&session, &to, &rusd, "0").is_err());
        assert!(draft_transfer(&session, &to, &rusd, "4611686018427387905").is_err());
        assert!(draft_transfer(&session, "nobody", &rusd, "25").is_err());
    }

    #[test]
    fn the_account_view_lists_every_asset_and_the_outbox() {
        let (session, _folder) = session("account");
        let account = session.create_key().unwrap();
        // No seat answered: nothing is claimed about the account.
        let silent = [SeatState {
            seat: 0,
            height: None,
            leaf: None,
            error: Some("cannot connect".into()),
        }];
        let view = account_view(&session, &silent);
        assert_eq!(view.account, Some(id_hex(&account)));
        assert_eq!(
            (view.height.clone(), view.proved, view.open_orders),
            (None, false, None)
        );
        assert_eq!(view.orders_mismatch, None);
        let names: Vec<&str> = view
            .balances
            .iter()
            .map(|b| b.asset.name.as_str())
            .collect();
        assert_eq!(names, ["B3", "TOK", "rUSD"]);
        assert!(view
            .balances
            .iter()
            .all(|b| b.available == "0" && b.reserved == "0"));
        // Seats answered that they do not know the account.
        let unknown = [SeatState {
            seat: 0,
            height: Some(77),
            leaf: None,
            error: None,
        }];
        let view = account_view(&session, &unknown);
        assert_eq!(
            (view.height.as_deref(), view.proved, view.open_orders),
            (Some("77"), false, Some(0))
        );
        assert!(!view.seats[0].proved);
    }

    fn account_view(session: &Session, states: &[SeatState]) -> AccountView {
        account(session, states, &BTreeMap::new())
    }
}
