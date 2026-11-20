//! The Trade page's commands: one connection to a FlowMesh V2 network
//! (fm2/), its live streams, and the drafts the approval window decides.
//!
//! The page chooses; the core checks, signs and verifies. An order, a
//! cancellation or a transfer is drafted here, shown in the approval window
//! and signed only when it is approved there. What the page is sent is
//! either verified against the genesis document or named as a seat's claim.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;

use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Emitter, Manager, State, Window};

use crate::drafts::Request;
use crate::fm2::bytes::{id_from_hex, id_hex, H256};
use crate::fm2::client::{network_tag, Channel, Progress, Session, StreamUpdate};
use crate::fm2::genesis::{Genesis, MAX_GENESIS_BYTES};
use crate::fm2::outbox::SeenFill;
use crate::fm2::reply::StreamBody;
use crate::fm2::types::SIDE_BUY;
use crate::fm2::verify::Trust;
use crate::fm2::view;
use crate::{blocking, hive_log, open_approval, require_window, Core, MAIN};

/// At most this many unsettled actions are asked about per pass, oldest
/// first: every one costs a request to every seat.
const SETTLE_BATCH: usize = 8;

/// One connection, from connect to disconnect.
pub(crate) struct Live {
    session: Session,
    /// What the seats last said about actions without a certified end.
    notes: Mutex<BTreeMap<String, String>>,
    /// One settle pass at a time.
    settling: Mutex<()>,
    /// Set when the connection ends.
    closed: AtomicBool,
    /// The page's current watch; set to stop its stream threads.
    watch: Mutex<Option<Arc<AtomicBool>>>,
}

impl Live {
    fn stop_watch(&self) {
        if let Some(stop) = self.watch.lock().unwrap().take() {
            stop.store(true, Ordering::SeqCst);
        }
    }

    fn close(&self) {
        self.closed.store(true, Ordering::SeqCst);
        self.stop_watch();
    }
}

#[derive(Default)]
pub(crate) struct Fm2State {
    live: Mutex<Option<Arc<Live>>>,
}

impl Fm2State {
    fn live(&self) -> Result<Arc<Live>, String> {
        self.live
            .lock()
            .unwrap()
            .clone()
            .ok_or_else(|| "B3 Hive is not connected to a FlowMesh V2 network.".to_owned())
    }

    /// The app is closing: every stream thread ends.
    pub(crate) fn shutdown(&self) {
        if let Some(live) = self.live.lock().unwrap().take() {
            live.close();
        }
    }
}

// --------------------------------------------------------------- connection

/// The files of the last connection, to offer them again.
#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub(crate) struct LastFiles {
    genesis: String,
    pins: String,
}

fn fm2_dir(app: &AppHandle) -> Result<PathBuf, String> {
    app.path()
        .app_config_dir()
        .map(|dir| dir.join("fm2"))
        .map_err(|e| format!("B3 Hive has no folder for its settings: {e}"))
}

/// A small trust file, read whole. A larger file is not one.
fn read_small(path: &str, what: &str) -> Result<String, String> {
    let path = Path::new(path.trim());
    let size = std::fs::metadata(path)
        .map_err(|e| match e.kind() {
            std::io::ErrorKind::NotFound => {
                format!("There is no {what} at {} any more.", path.display())
            }
            _ => format!("Cannot read the {what} {}: {e}", path.display()),
        })?
        .len();
    if size > MAX_GENESIS_BYTES {
        return Err(format!("{} is too large to be a {what}.", path.display()));
    }
    std::fs::read_to_string(path)
        .map_err(|e| format!("Cannot read the {what} {}: {e}", path.display()))
}

/// The last connection's files, to prefill the form.
#[tauri::command]
pub(crate) fn fm2_last(app: AppHandle, window: Window) -> Result<Option<LastFiles>, String> {
    require_window(&window, MAIN)?;
    let file = fm2_dir(&app)?.join("last.json");
    Ok(std::fs::read(file)
        .ok()
        .and_then(|bytes| serde_json::from_slice(&bytes).ok()))
}

/// Asks for a network's folder, or for a genesis document or a pins file,
/// with the native picker.
#[tauri::command]
pub(crate) async fn fm2_pick(
    app: AppHandle,
    window: Window,
    what: String,
) -> Result<Option<String>, String> {
    require_window(&window, MAIN)?;
    blocking(move || {
        use tauri_plugin_dialog::DialogExt;
        let title = match what.as_str() {
            "folder" => "Choose the test network's folder",
            "genesis" => "Choose the network's genesis document",
            "pins" => "Choose the network's pins file",
            _ => return Err("Unknown file.".to_owned()),
        };
        let mut dialog = app.dialog().file().set_title(title);
        if let Some(main) = app.get_webview_window(MAIN) {
            dialog = dialog.set_parent(&main);
        }
        let picked = if what == "folder" {
            dialog.blocking_pick_folder()
        } else {
            dialog
                .add_filter("JSON files", &["json"])
                .blocking_pick_file()
        };
        Ok(picked
            .and_then(|file| file.into_path().ok())
            .map(|path| path.display().to_string()))
    })
    .await
}

/// The two trust files in a network's folder. An operator keeps them side
/// by side under these names (src/flowmesh2/README.md), so one folder is
/// all the user has to choose.
fn locate(folder: &Path) -> Result<LastFiles, String> {
    let find = |name: &str| {
        let path = folder.join(name);
        path.is_file().then(|| path.display().to_string())
    };
    match (find("genesis.json"), find("pins.json")) {
        (Some(genesis), Some(pins)) => Ok(LastFiles { genesis, pins }),
        (None, None) => Err(format!(
            "{} holds neither genesis.json nor pins.json. Choose the folder you got from whoever runs the network.",
            folder.display()
        )),
        (None, Some(_)) => Err(format!(
            "{} has a pins.json but no genesis.json. Ask whoever runs the network for both files.",
            folder.display()
        )),
        (Some(_), None) => Err(format!(
            "{} has a genesis.json but no pins.json. Ask whoever runs the network for both files.",
            folder.display()
        )),
    }
}

#[tauri::command]
pub(crate) fn fm2_locate(window: Window, folder: String) -> Result<LastFiles, String> {
    require_window(&window, MAIN)?;
    locate(Path::new(folder.trim()))
}

/// Connects to the network of a genesis document, with the seats' pins.
/// Both files are trust inputs the user got from the network's operator;
/// nothing is taken from a seat. The account's key and outbox live in B3
/// Hive's own folder, one per network. Nothing is sent by connecting except
/// status questions about actions the outbox already holds.
#[tauri::command]
pub(crate) async fn fm2_connect(
    app: AppHandle,
    window: Window,
    core: State<'_, Arc<Core>>,
    genesis: String,
    pins: String,
) -> Result<view::Info, String> {
    require_window(&window, MAIN)?;
    let core = Arc::clone(&core);
    blocking(move || {
        let genesis_json = read_small(&genesis, "genesis document")?;
        let pins_json = read_small(&pins, "pins file")?;
        let tag = Genesis::parse(&genesis_json)
            .and_then(|parsed| Trust::from_genesis(&parsed))
            .map(|trust| network_tag(&trust))
            .map_err(String::from)?;
        let root = fm2_dir(&app)?;
        // The old connection lets go of its outbox before the new one opens
        // it: the same network may be connected again.
        if let Some(old) = core.fm2.live.lock().unwrap().take() {
            old.close();
        }
        let session =
            Session::open(&genesis_json, &pins_json, &root.join(&tag)).map_err(String::from)?;
        let info = view::info(&session, session.recovered_bytes());
        hive_log(
            &app,
            &format!(
                "fm2: connected to {} ({} seats), account folder {tag}",
                info.domain,
                info.seats.len()
            ),
        );
        let live = Arc::new(Live {
            session,
            notes: Mutex::default(),
            settling: Mutex::default(),
            closed: AtomicBool::new(false),
            watch: Mutex::default(),
        });
        *core.fm2.live.lock().unwrap() = Some(Arc::clone(&live));
        let last = LastFiles {
            genesis: genesis.trim().to_owned(),
            pins: pins.trim().to_owned(),
        };
        if let Ok(text) = serde_json::to_vec_pretty(&last) {
            let _ = std::fs::write(root.join("last.json"), text);
        }
        // Actions signed in an earlier run: ask for their ends, and post
        // again what no seat holds.
        thread::spawn(move || settle_pass(&app, &live, true));
        Ok(info)
    })
    .await
}

#[tauri::command]
pub(crate) fn fm2_disconnect(window: Window, core: State<'_, Arc<Core>>) -> Result<(), String> {
    require_window(&window, MAIN)?;
    if let Some(live) = core.fm2.live.lock().unwrap().take() {
        live.close();
    }
    Ok(())
}

/// The connection, if there is one (the page was reopened).
#[tauri::command]
pub(crate) fn fm2_info(
    window: Window,
    core: State<'_, Arc<Core>>,
) -> Result<Option<view::Info>, String> {
    require_window(&window, MAIN)?;
    let live = core.fm2.live.lock().unwrap().clone();
    Ok(live.map(|live| view::info(&live.session, live.session.recovered_bytes())))
}

/// Creates this network's key in B3 Hive's folder and returns its account.
/// The key file is not encrypted: FlowMesh V2 is a regtest network.
#[tauri::command]
pub(crate) async fn fm2_key_create(
    app: AppHandle,
    window: Window,
    core: State<'_, Arc<Core>>,
) -> Result<String, String> {
    require_window(&window, MAIN)?;
    let live = core.fm2.live()?;
    blocking(move || {
        let account = live.session.create_key().map_err(String::from)?;
        hive_log(&app, &format!("fm2: created account {}", id_hex(&account)));
        Ok(id_hex(&account))
    })
    .await
}

#[derive(Serialize)]
pub(crate) struct SeatHealth {
    seat: u32,
    /// The seat answered over its pinned connection.
    reachable: bool,
    fenced: bool,
    published_height: Option<String>,
    pool_live: Option<String>,
    error: Option<String>,
}

/// Every seat's own diagnostics. A seat's claim, never certified.
#[tauri::command]
pub(crate) async fn fm2_health(
    window: Window,
    core: State<'_, Arc<Core>>,
) -> Result<Vec<SeatHealth>, String> {
    require_window(&window, MAIN)?;
    let live = core.fm2.live()?;
    blocking(move || {
        Ok(live
            .session
            .health()
            .into_iter()
            .map(|(seat, answer)| match answer {
                Ok(health) => SeatHealth {
                    seat,
                    reachable: true,
                    fenced: health.fenced,
                    published_height: health.published_height.map(|h| h.to_string()),
                    pool_live: Some(health.pool_live.to_string()),
                    error: None,
                },
                Err(error) => SeatHealth {
                    seat,
                    reachable: false,
                    fenced: false,
                    published_height: None,
                    pool_live: None,
                    error: Some(error.to_string()),
                },
            })
            .collect())
    })
    .await
}

/// The account as every seat proves it now, the open orders rebuilt from
/// the outbox, and the outbox itself.
#[tauri::command]
pub(crate) async fn fm2_account(
    window: Window,
    core: State<'_, Arc<Core>>,
) -> Result<view::AccountView, String> {
    require_window(&window, MAIN)?;
    let live = core.fm2.live()?;
    blocking(move || {
        let states = match live.session.account() {
            Some(account) => live.session.state(&account),
            None => Vec::new(),
        };
        let notes = live.notes.lock().unwrap().clone();
        Ok(view::account(&live.session, &states, &notes))
    })
    .await
}

// ------------------------------------------------------------------ streams

#[derive(Clone, Serialize)]
struct LinkEvent {
    channel: &'static str,
    seat: u32,
    live: bool,
    detail: String,
}

#[derive(Clone, Serialize)]
struct SlotEvent {
    seat: u32,
    height: String,
    actions: u64,
    outcomes: u64,
}

#[derive(Clone, Serialize)]
struct Level {
    price_ticks: String,
    lots: String,
    orders: u64,
}

#[derive(Clone, Serialize)]
struct BookEvent {
    seat: u32,
    market: String,
    slot: String,
    bids: Vec<Level>,
    asks: Vec<Level>,
}

#[derive(Clone, Serialize)]
struct TradeFill {
    taker_side: &'static str,
    price_ticks: String,
    lots: String,
    quote_atoms: String,
    /// This account's part in the fill: "maker", "taker" or neither.
    mine: Option<&'static str>,
}

#[derive(Clone, Serialize)]
struct TradesEvent {
    seat: u32,
    market: String,
    slot: String,
    fills: Vec<TradeFill>,
}

#[derive(Clone, Serialize)]
struct AccountEvent {
    seat: u32,
    slot: String,
}

fn levels(list: &[crate::fm2::reply::BookLevel]) -> Vec<Level> {
    list.iter()
        .map(|level| Level {
            price_ticks: level.price_ticks.to_string(),
            lots: level.lots.to_string(),
            orders: level.orders,
        })
        .collect()
}

/// Follows one channel until the watch ends, sending what arrives to the
/// main window.
fn follow(app: AppHandle, live: Arc<Live>, stop: Arc<AtomicBool>, channel: Channel) {
    let name = match channel {
        Channel::Slots => "slots",
        Channel::Account(_) => "account",
        Channel::Book(_) => "book",
        Channel::Trades(_) => "trades",
    };
    let session = &live.session;
    session.follow(channel, &stop, |update| {
        if stop.load(Ordering::SeqCst) {
            return;
        }
        let (seat, event) = match update {
            StreamUpdate::Link {
                seat,
                live: up,
                detail,
            } => {
                let link = LinkEvent {
                    channel: name,
                    seat,
                    live: up,
                    detail,
                };
                let _ = app.emit_to(MAIN, "fm2-link", link);
                return;
            }
            StreamUpdate::Event { seat, event, .. } => (seat, event),
        };
        let slot = event.head_slot.to_string();
        match event.body {
            // `follow` passes on a slot event only when its certificate held.
            StreamBody::Slot {
                envelope,
                action_count,
                outcome_count,
            } => {
                let slot = SlotEvent {
                    seat,
                    height: envelope.header.height.to_string(),
                    actions: action_count,
                    outcomes: outcome_count,
                };
                let _ = app.emit_to(MAIN, "fm2-slot", slot);
                // A new certified slot may hold the end of a waiting action.
                if !session.open_labels().is_empty() {
                    let (app, live) = (app.clone(), Arc::clone(&live));
                    thread::spawn(move || settle_pass(&app, &live, false));
                }
            }
            StreamBody::Account { .. } => {
                // The leaf in the event carries no proof; the page asks for
                // the proved one.
                let _ = app.emit_to(MAIN, "fm2-account", AccountEvent { seat, slot });
            }
            StreamBody::Book { market, bids, asks } => {
                let book = BookEvent {
                    seat,
                    market: id_hex(&market),
                    slot,
                    bids: levels(&bids),
                    asks: levels(&asks),
                };
                let _ = app.emit_to(MAIN, "fm2-book", book);
            }
            StreamBody::Trades { market, fills } => {
                let own = session.action_ids();
                let seen: Vec<SeenFill> = fills
                    .iter()
                    .map(|fill| SeenFill {
                        order: fill.maker,
                        taker: fill.taker,
                        slot: event.head_slot,
                        price_ticks: fill.price_ticks,
                        lots: fill.lots,
                    })
                    .collect();
                // Fills of this account's resting orders are kept, so the
                // open orders can be rebuilt after a restart.
                match session.note_fills(&seen) {
                    Ok(true) => {
                        let _ = app.emit_to(MAIN, "fm2-activity", ());
                    }
                    Ok(false) => {}
                    Err(error) => hive_log(&app, &format!("fm2: {error}")),
                }
                let trades = TradesEvent {
                    seat,
                    market: id_hex(&market),
                    slot,
                    fills: fills
                        .iter()
                        .map(|fill| TradeFill {
                            taker_side: if fill.taker_side == SIDE_BUY {
                                "buy"
                            } else {
                                "sell"
                            },
                            price_ticks: fill.price_ticks.to_string(),
                            lots: fill.lots.to_string(),
                            quote_atoms: fill.quote_atoms.to_string(),
                            mine: if own.contains(&fill.maker) {
                                Some("maker")
                            } else if own.contains(&fill.taker) {
                                Some("taker")
                            } else {
                                None
                            },
                        })
                        .collect(),
                };
                let _ = app.emit_to(MAIN, "fm2-trades", trades);
            }
        }
    });
}

/// Starts the page's live view of one market: certified slots, the market's
/// book and trades, and this account's changes. It replaces an earlier
/// watch.
#[tauri::command]
pub(crate) fn fm2_watch(
    app: AppHandle,
    window: Window,
    core: State<'_, Arc<Core>>,
    market: String,
) -> Result<(), String> {
    require_window(&window, MAIN)?;
    let live = core.fm2.live()?;
    let market: H256 = id_from_hex(&market).ok_or("The market is not an id.")?;
    if live.session.genesis.market(&market).is_none() {
        return Err("This market is not in the genesis document.".into());
    }
    let stop = Arc::new(AtomicBool::new(false));
    {
        let mut watch = live.watch.lock().unwrap();
        if let Some(old) = watch.replace(Arc::clone(&stop)) {
            old.store(true, Ordering::SeqCst);
        }
    }
    let mut channels = vec![
        Channel::Slots,
        Channel::Book(market),
        Channel::Trades(market),
    ];
    if let Some(account) = live.session.account() {
        channels.push(Channel::Account(account));
    }
    for channel in channels {
        let (app, live, stop) = (app.clone(), Arc::clone(&live), Arc::clone(&stop));
        thread::spawn(move || follow(app, live, stop, channel));
    }
    Ok(())
}

/// The Trade page was left: its streams end; the connection stays.
#[tauri::command]
pub(crate) fn fm2_unwatch(window: Window, core: State<'_, Arc<Core>>) -> Result<(), String> {
    require_window(&window, MAIN)?;
    if let Some(live) = core.fm2.live.lock().unwrap().as_ref() {
        live.stop_watch();
    }
    Ok(())
}

// ----------------------------------------------------------------- settling

fn seats(list: &[u32]) -> String {
    let names: Vec<String> = list.iter().map(u32::to_string).collect();
    names.join(", ")
}

/// Asks every seat for the end of each waiting action, oldest first, and
/// records the ends that are proved. With `resend`, an action no seat holds
/// is posted again: the same signed bytes, never a new signature.
fn settle_pass(app: &AppHandle, live: &Live, resend: bool) {
    let Ok(_one) = live.settling.try_lock() else {
        return;
    };
    let session = &live.session;
    let mut changed = false;
    for label in session.open_labels().into_iter().take(SETTLE_BATCH) {
        if live.closed.load(Ordering::SeqCst) {
            return;
        }
        let note = match session.settle(&label) {
            Ok(Progress::Included { .. } | Progress::Expired { .. }) => None,
            Ok(Progress::Failed) => Some(
                "Every seat says its pool dropped this action. That is not certified; it stays here until a seat proves it expired."
                    .to_owned(),
            ),
            Ok(Progress::Waiting {
                pending,
                refused_claims,
                detail,
            }) => {
                let mut note = if pending.is_empty() {
                    "No seat reports it in its pool.".to_owned()
                } else {
                    format!("Waiting in the pool of seat {}.", seats(&pending))
                };
                if refused_claims > 0 {
                    note.push_str(&format!(
                        " {refused_claims} seat(s) claimed an end without a valid proof; that was not used."
                    ));
                }
                if !detail.is_empty() {
                    note.push_str(&format!(" ({detail})"));
                }
                if pending.is_empty() && resend {
                    if let Ok(admissions) = session.resend(&label) {
                        let holding: Vec<u32> = admissions
                            .iter()
                            .filter(|a| a.accepted())
                            .map(|a| a.seat)
                            .collect();
                        note = if holding.is_empty() {
                            format!("{note} Posted again; no seat took it.")
                        } else {
                            format!("Posted again; seat {} holds it.", seats(&holding))
                        };
                    }
                }
                Some(note)
            }
            Err(error) => Some(error.to_string()),
        };
        let mut notes = live.notes.lock().unwrap();
        let before = notes.get(&label).cloned();
        match &note {
            Some(text) => notes.insert(label.clone(), text.clone()),
            None => notes.remove(&label),
        };
        changed |= note.is_none() || before != note;
    }
    if changed {
        let _ = app.emit_to(MAIN, "fm2-activity", ());
    }
}

/// Asks the seats again about every waiting action, now.
#[tauri::command]
pub(crate) async fn fm2_refresh(
    app: AppHandle,
    window: Window,
    core: State<'_, Arc<Core>>,
) -> Result<(), String> {
    require_window(&window, MAIN)?;
    let live = core.fm2.live()?;
    blocking(move || {
        settle_pass(&app, &live, false);
        Ok(())
    })
    .await
}

/// Posts a stored action again, exactly as it was signed. A seat that
/// already holds or included it answers DUPLICATE.
#[tauri::command]
pub(crate) async fn fm2_resend(
    app: AppHandle,
    window: Window,
    core: State<'_, Arc<Core>>,
    label: String,
) -> Result<Vec<view::Admission>, String> {
    require_window(&window, MAIN)?;
    let live = core.fm2.live()?;
    blocking(move || {
        let admissions = live.session.resend(&label).map_err(String::from)?;
        hive_log(&app, &format!("fm2: posted {label} again"));
        Ok(view::admissions(&admissions))
    })
    .await
}

// ------------------------------------------------------------------- drafts

/// Puts a checked draft in front of the user.
fn offer(app: &AppHandle, core: &Core, action: view::ActionDraft) -> Result<String, String> {
    let id = core
        .drafts
        .lock()
        .unwrap()
        .insert(Request::Fm2 {
            action: Box::new(action),
        })
        .map_err(|e| e.to_string())?;
    open_approval(app).map_err(|e| e.to_string())?;
    Ok(id)
}

/// A limit order as the page's ticket gives it.
#[derive(Deserialize)]
pub(crate) struct Ticket {
    market: String,
    side: String,
    tif: String,
    price_ticks: String,
    lots: String,
}

/// Drafts a limit order for the approval window. The page only learns the
/// draft id.
#[tauri::command]
pub(crate) async fn fm2_place_draft(
    app: AppHandle,
    window: Window,
    core: State<'_, Arc<Core>>,
    order: Ticket,
) -> Result<String, String> {
    require_window(&window, MAIN)?;
    let live = core.fm2.live()?;
    let action = view::draft_place(
        &live.session,
        &order.market,
        &order.side,
        &order.tif,
        &order.price_ticks,
        &order.lots,
    )?;
    offer(&app, &core, action)
}

#[tauri::command]
pub(crate) async fn fm2_cancel_draft(
    app: AppHandle,
    window: Window,
    core: State<'_, Arc<Core>>,
    order: String,
) -> Result<String, String> {
    require_window(&window, MAIN)?;
    let live = core.fm2.live()?;
    let action = view::draft_cancel(&live.session, &order)?;
    offer(&app, &core, action)
}

#[tauri::command]
pub(crate) async fn fm2_cancel_all_draft(
    app: AppHandle,
    window: Window,
    core: State<'_, Arc<Core>>,
) -> Result<String, String> {
    require_window(&window, MAIN)?;
    let live = core.fm2.live()?;
    let action = view::draft_cancel_all(&live.session)?;
    offer(&app, &core, action)
}

#[tauri::command]
pub(crate) async fn fm2_transfer_draft(
    app: AppHandle,
    window: Window,
    core: State<'_, Arc<Core>>,
    to: String,
    asset: String,
    amount: String,
) -> Result<String, String> {
    require_window(&window, MAIN)?;
    let live = core.fm2.live()?;
    let action = view::draft_transfer(&live.session, &to, &asset, &amount)?;
    offer(&app, &core, action)
}

/// Carries out an approved draft: signs the action once, stores it and
/// posts it to every seat. The draft was made for one network and one
/// account; under any other connection it is refused, unsigned.
pub(crate) fn execute(
    app: &AppHandle,
    core: &Core,
    action: view::ActionDraft,
) -> Result<view::Sent, String> {
    let live = core.fm2.live()?;
    let session = &live.session;
    if (session.trust.domain, session.account()) != (action.bound.0, Some(action.bound.1)) {
        return Err(
            "The connection changed since this was drafted. Nothing was signed; draft it again."
                .into(),
        );
    }
    let submitted = session
        .submit(&action.payload, action.window_slots)
        .map_err(String::from)?;
    let sent = view::sent(&submitted);
    hive_log(
        app,
        &format!(
            "fm2: signed {} ({}), {} of {} seats hold it",
            sent.label,
            sent.action_id,
            sent.accepted,
            sent.admissions.len()
        ),
    );
    let (app, live) = (app.clone(), Arc::clone(&live));
    thread::spawn(move || settle_pass(&app, &live, false));
    Ok(sent)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_network_folder_must_hold_both_files() {
        let dir = std::env::temp_dir().join(format!("hive-fm2-locate-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        assert!(locate(&dir).unwrap_err().contains("neither"));
        std::fs::write(dir.join("genesis.json"), "{}").unwrap();
        assert!(locate(&dir).unwrap_err().contains("no pins.json"));
        std::fs::write(dir.join("pins.json"), "{}").unwrap();
        let found = locate(&dir).unwrap();
        assert!(found.genesis.ends_with("genesis.json"));
        assert!(found.pins.ends_with("pins.json"));
        std::fs::remove_file(dir.join("genesis.json")).unwrap();
        assert!(locate(&dir).unwrap_err().contains("no genesis.json"));
        // A folder of that name is not the file.
        std::fs::create_dir(dir.join("genesis.json")).unwrap();
        assert!(locate(&dir).is_err());
        let _ = std::fs::remove_dir_all(&dir);
    }
}
