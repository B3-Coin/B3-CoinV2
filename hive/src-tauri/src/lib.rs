//! The trusted core of the B3 Hive app. Windows only ask; the core holds the
//! node credentials, builds every transaction itself, and submits one only
//! after the approval window approves it. The one exception is the console
//! window: commands typed there run with full access, as in the Qt console.

pub mod book;
pub mod bootstrap;
pub mod chain;
pub mod console;
pub mod drafts;
pub mod flowmesh;
pub mod history;
pub mod node;
pub mod passphrase;
pub mod psbt;
pub mod receive;
pub mod rpc;
pub mod settings;
pub mod stake;
pub mod update;
pub mod wallet;

use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use std::thread;
use std::time::Duration;

use serde::{Deserialize, Serialize};
use tauri::{
    AppHandle, Emitter, Manager, RunEvent, State, WebviewUrl, WebviewWindowBuilder, Window,
    WindowEvent,
};

use drafts::{DraftStore, DraftView, Outpoint, Request};
use node::{Network, Node, NodeConfig, NodeStatus};
use rpc::RpcError;
use update::{HttpsFetcher, UpdateStatus, Updater};

const START_TIMEOUT: Duration = Duration::from_secs(600);
const STOP_TIMEOUT: Duration = Duration::from_secs(120);
/// How often the running node's tip is read for the main window.
const FOLLOW_INTERVAL: Duration = Duration::from_secs(2);
const MAIN: &str = "main";
const APPROVAL: &str = "approval";
const CONSOLE: &str = "console";
const PASSPHRASE: &str = "passphrase";

pub struct Core {
    /// The current node. A restart swaps in a new one; callers take a clone
    /// with `node()` and keep using it for one command.
    node: Mutex<Arc<Node>>,
    config: NodeConfig,
    /// Set once the app is exiting, so a late restart cannot leave a node behind.
    exiting: AtomicBool,
    drafts: Mutex<DraftStore>,
    /// The wallet picked in the main window; the pages fall back to the first
    /// loaded wallet when it is not loaded.
    wallet: Mutex<Option<String>>,
    /// The passphrase window's request while it is open.
    passphrase: Mutex<Option<passphrase::Request>>,
    /// The theme picked in the main window, so the other windows open in it.
    theme: Mutex<Option<Theme>>,
    updates: Updates,
    setup: Setup,
}

/// The setup wizard's state: the explorer's last offers, and the bootstrap
/// install while one runs.
#[derive(Default)]
struct Setup {
    offers: Mutex<Vec<bootstrap::Offer>>,
    cancel: Arc<AtomicBool>,
    install: Mutex<Option<thread::JoinHandle<()>>>,
}

impl Setup {
    fn installing(&self) -> bool {
        self.install
            .lock()
            .unwrap()
            .as_ref()
            .is_some_and(|h| !h.is_finished())
    }

    /// Cancels a running install and waits for it, so the app never exits
    /// halfway through moving the chain folders into place.
    fn stop(&self) {
        self.cancel.store(true, Ordering::SeqCst);
        if let Some(handle) = self.install.lock().unwrap().take() {
            let _ = handle.join();
        }
    }
}

impl Core {
    fn node(&self) -> Arc<Node> {
        Arc::clone(&self.node.lock().unwrap())
    }
}

#[derive(Clone, Copy, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Theme {
    Light,
    Dark,
}

#[derive(Clone, Serialize)]
pub struct WalletOverview {
    pub wallet: String,
    pub spendable: String,
    pub pending: String,
    pub immature: String,
    pub security: wallet::Security,
    /// None with staking_error when getstakinginfo failed.
    pub staking: Option<wallet::Staking>,
    pub staking_error: Option<String>,
    /// The funding button is offered on regtest only.
    pub can_fund: bool,
}

/// Sent to the main window when the passphrase window finishes. The main
/// window learns only the outcome, never the passphrase.
#[derive(Clone, Serialize)]
#[serde(tag = "outcome", rename_all = "lowercase")]
pub enum PassResult {
    Done {
        kind: passphrase::Kind,
        wallet: String,
    },
    Cancelled {
        kind: passphrase::Kind,
    },
}

/// Sent to the main window when a draft is decided.
#[derive(Clone, Serialize)]
#[serde(tag = "outcome", rename_all = "lowercase")]
pub enum DraftResult {
    Sent {
        id: String,
        txid: String,
    },
    /// A PSBT was signed; it goes back to the page, not to the network.
    Signed {
        id: String,
        psbt: String,
        complete: bool,
    },
    Rejected {
        id: String,
    },
    Failed {
        id: String,
        error: String,
    },
}

// ---- main window commands --------------------------------------------------

#[tauri::command]
fn node_status(core: State<'_, Arc<Core>>) -> NodeStatus {
    core.node().status()
}

/// Starts the node again after it failed to start or exited. Refused while
/// the old process still runs, so two nodes never share the data folder.
#[tauri::command]
fn restart_node(app: AppHandle, window: Window, core: State<'_, Arc<Core>>) -> Result<(), String> {
    require_window(&window, MAIN)?;
    let mut current = core.node.lock().unwrap();
    if core.exiting.load(Ordering::SeqCst) {
        return Err("B3 Hive is closing.".into());
    }
    if !current.has_exited() {
        return Err("The node is still running.".into());
    }
    *current = start_node(&app, &core.config);
    Ok(())
}

/// The wallet the pages use, or an error the page can show. Mainnet uses
/// the first wallet the node loaded; the picker comes with phase 3.
fn page_wallet<'a>(core: &Core, node: &'a Node) -> Result<(&'a rpc::Client, String), String> {
    let client = node.client().ok_or("The node is not ready yet.")?;
    let chain = node.chain().unwrap_or_default();
    let preferred = core.wallet.lock().unwrap().clone();
    match wallet::select(client, &chain, preferred.as_deref()).map_err(|e| e.to_string())? {
        Some(name) => Ok((client, name)),
        None => Err(NO_WALLET.into()),
    }
}

const NO_WALLET: &str = "No wallet is loaded.";

/// None when the node has no wallet loaded.
#[tauri::command]
async fn wallet_overview(core: State<'_, Arc<Core>>) -> Result<Option<WalletOverview>, String> {
    let core = Arc::clone(&core);
    blocking(move || {
        let node = core.node();
        let (client, name) = match page_wallet(&core, &node) {
            Ok(found) => found,
            Err(e) if e == NO_WALLET => return Ok(None),
            Err(e) => return Err(e),
        };
        let balances = wallet::balances(client, &name).map_err(|e| e.to_string())?;
        let security = wallet::security(client, &name).map_err(|e| e.to_string())?;
        // Qt shows the rest of the page when the staking status fails.
        let (staking, staking_error) = match wallet::staking(client, &name) {
            Ok(s) => (Some(s), None),
            Err(e) => (None, Some(e.to_string())),
        };
        Ok(Some(WalletOverview {
            wallet: name,
            spendable: balances.spendable,
            pending: balances.pending,
            immature: balances.immature,
            security,
            staking,
            staking_error,
            can_fund: node.chain().as_deref() == Some("regtest"),
        }))
    })
    .await
}

#[tauri::command]
async fn dev_fund(core: State<'_, Arc<Core>>) -> Result<(), String> {
    let core = Arc::clone(&core);
    blocking(move || {
        let node = core.node();
        if node.chain().as_deref() != Some("regtest") {
            return Err("Funding by mining is only available on regtest.".into());
        }
        let (client, name) = page_wallet(&core, &node)?;
        wallet::fund_regtest(client, &name).map_err(|e| e.to_string())
    })
    .await
}

/// The wallet's newest transactions, newest first.
#[tauri::command]
async fn wallet_activity(
    core: State<'_, Arc<Core>>,
    count: u64,
) -> Result<Vec<wallet::ActivityEntry>, String> {
    let core = Arc::clone(&core);
    blocking(move || {
        let node = core.node();
        let (client, name) = page_wallet(&core, &node)?;
        wallet::activity(client, &name, count).map_err(|e| e.to_string())
    })
    .await
}

#[derive(Serialize)]
pub struct NodeInfo {
    #[serde(flatten)]
    pub details: chain::NodeDetails,
    pub datadir: String,
    pub debug_log: String,
}

/// The traffic graph's sample.
#[tauri::command]
async fn network_totals(core: State<'_, Arc<Core>>) -> Result<chain::Totals, String> {
    let core = Arc::clone(&core);
    blocking(move || {
        let node = core.node();
        let client = node.client().ok_or("The node is not ready yet.")?;
        chain::totals(client).map_err(|e| e.to_string())
    })
    .await
}

/// The Network page's Information rows and banned peers.
#[tauri::command]
async fn node_info(core: State<'_, Arc<Core>>) -> Result<NodeInfo, String> {
    let core = Arc::clone(&core);
    blocking(move || {
        let node = core.node();
        let client = node.client().ok_or("The node is not ready yet.")?;
        let chain_dir = core.config.network.chain_dir(&core.config.datadir);
        Ok(NodeInfo {
            details: chain::details(client).map_err(|e| e.to_string())?,
            datadir: core.config.datadir.display().to_string(),
            debug_log: chain_dir.join("debug.log").display().to_string(),
        })
    })
    .await
}

/// The Activity page: the newest `count` lines that pass the filter.
#[tauri::command]
async fn wallet_history(
    core: State<'_, Arc<Core>>,
    filter: history::Filter,
    count: usize,
) -> Result<history::Page, String> {
    let core = Arc::clone(&core);
    blocking(move || {
        let node = core.node();
        let (client, name) = page_wallet(&core, &node)?;
        let lines = history::all(client, &name).map_err(|e| e.to_string())?;
        history::filter(lines, &filter, count)
    })
    .await
}

#[tauri::command]
async fn wallet_transaction(
    core: State<'_, Arc<Core>>,
    txid: String,
) -> Result<history::Details, String> {
    let core = Arc::clone(&core);
    blocking(move || {
        let node = core.node();
        let (client, name) = page_wallet(&core, &node)?;
        history::details(client, &name, &txid)
    })
    .await
}

/// Writes the lines that pass the filter as Qt's CSV into the Downloads
/// folder, under a new name, and returns where.
#[tauri::command]
async fn wallet_export_csv(
    app: AppHandle,
    window: Window,
    core: State<'_, Arc<Core>>,
    filter: history::Filter,
) -> Result<String, String> {
    require_window(&window, MAIN)?;
    let folder = app
        .path()
        .download_dir()
        .map_err(|e| format!("The Downloads folder was not found: {e}"))?;
    let core = Arc::clone(&core);
    blocking(move || {
        let node = core.node();
        let (client, name) = page_wallet(&core, &node)?;
        let lines = history::all(client, &name).map_err(|e| e.to_string())?;
        let page = history::filter(lines, &filter, usize::MAX)?;
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_or(0, |d| d.as_secs() as i64);
        let path = folder.join(format!(
            "b3-hive-transactions-{}.csv",
            history::utc(now).replace(':', "")
        ));
        let mut file = std::fs::File::create_new(&path)
            .map_err(|e| format!("Cannot create {}: {e}", path.display()))?;
        std::io::Write::write_all(&mut file, history::csv(&page.entries).as_bytes())
            .map_err(|e| format!("Cannot write {}: {e}", path.display()))?;
        Ok(path.display().to_string())
    })
    .await
}

// ---- receive (main window) -----------------------------------------------------

/// Qt's "Create new receiving address".
#[tauri::command]
async fn receive_new(
    window: Window,
    core: State<'_, Arc<Core>>,
    label: String,
    amount: String,
    message: String,
) -> Result<receive::Request, String> {
    require_window(&window, MAIN)?;
    let core = Arc::clone(&core);
    blocking(move || {
        let node = core.node();
        let (client, name) = page_wallet(&core, &node)?;
        let chain = node.chain().unwrap_or_default();
        receive::new_request(client, &name, &chain, &label, &amount, &message)
    })
    .await
}

/// The page wallet's address book.
#[tauri::command]
async fn book_list(core: State<'_, Arc<Core>>) -> Result<Vec<book::Contact>, String> {
    let core = Arc::clone(&core);
    blocking(move || {
        let node = core.node();
        let (client, name) = page_wallet(&core, &node)?;
        book::contacts(client, &name).map_err(|e| e.to_string())
    })
    .await
}

/// Adds an address to the page wallet's address book, or renames it.
#[tauri::command]
async fn book_save(
    core: State<'_, Arc<Core>>,
    address: String,
    label: String,
) -> Result<book::Contact, String> {
    let core = Arc::clone(&core);
    blocking(move || {
        let node = core.node();
        let (client, name) = page_wallet(&core, &node)?;
        book::save(client, &name, &address, &label)
    })
    .await
}

/// The FlowMesh markets the node knows.
#[tauri::command]
async fn trade_markets(core: State<'_, Arc<Core>>) -> Result<Vec<flowmesh::MarketSummary>, String> {
    let core = Arc::clone(&core);
    blocking(move || {
        let node = core.node();
        let (client, name) = page_wallet(&core, &node)?;
        flowmesh::markets(client, &name).map_err(|e| e.to_string())
    })
    .await
}

/// One market's verified view for the page wallet. With `known_head` and
/// `watch` it is a live read: the node waits up to 1.5 s for the head to
/// move and leaves the heavy parts out when it has not.
#[tauri::command]
async fn trade_market(
    core: State<'_, Arc<Core>>,
    market_id: String,
    known_head: Option<String>,
    watch: bool,
    ticks_after: Option<u64>,
) -> Result<flowmesh::MarketView, String> {
    let core = Arc::clone(&core);
    blocking(move || {
        let node = core.node();
        let (client, name) = page_wallet(&core, &node)?;
        let query = flowmesh::Query {
            known_head,
            watch,
            ticks_after,
        };
        flowmesh::market(client, &name, &market_id, &query)
    })
    .await
}

/// The page wallet's staking status.
#[tauri::command]
async fn stake_status(core: State<'_, Arc<Core>>) -> Result<stake::Status, String> {
    let core = Arc::clone(&core);
    blocking(move || {
        let node = core.node();
        let (client, name) = page_wallet(&core, &node)?;
        stake::status(client, &name).map_err(|e| e.to_string())
    })
    .await
}

/// Asks the approval window to lock `amount` as a stake. Nothing is sent
/// here; the page only learns the draft id.
#[tauri::command]
async fn stake_draft(
    app: AppHandle,
    core: State<'_, Arc<Core>>,
    amount: String,
) -> Result<String, String> {
    let core = Arc::clone(&core);
    let id = blocking(move || {
        let node = core.node();
        let (client, name) = page_wallet(&core, &node)?;
        let amount = stake::check_stake(client, &name, &amount)?;
        let depth = stake::status(client, &name)
            .map_err(|e| e.to_string())?
            .activation_depth;
        let request = Request::Stake {
            chain: node.chain().unwrap_or_default(),
            wallet: name,
            amount,
            activation_depth: depth,
        };
        core.drafts
            .lock()
            .unwrap()
            .insert(request)
            .map_err(|e| e.to_string())
    })
    .await?;
    open_approval(&app).map_err(|e| e.to_string())?;
    Ok(id)
}

/// Reads a PSBT made elsewhere, for the PSBT page.
#[tauri::command]
async fn psbt_inspect(
    window: Window,
    core: State<'_, Arc<Core>>,
    psbt: String,
) -> Result<psbt::View, String> {
    require_window(&window, MAIN)?;
    let core = Arc::clone(&core);
    blocking(move || {
        let node = core.node();
        let (client, name) = page_wallet(&core, &node)?;
        let chain = node.chain().unwrap_or_default();
        let (summary, loaded) = psbt::inspect(client, &name, &chain, &psbt)?;
        Ok(psbt::View {
            psbt: loaded.psbt,
            summary,
        })
    })
    .await
}

/// Asks the approval window to sign a PSBT (`send` false) or broadcast a
/// complete one (`send` true).
#[tauri::command]
async fn psbt_draft(
    app: AppHandle,
    window: Window,
    core: State<'_, Arc<Core>>,
    psbt: String,
    send: bool,
) -> Result<String, String> {
    require_window(&window, MAIN)?;
    let core = Arc::clone(&core);
    let id = blocking(move || {
        let node = core.node();
        let (client, name) = page_wallet(&core, &node)?;
        let chain = node.chain().unwrap_or_default();
        let (summary, loaded) = psbt::inspect(client, &name, &chain, &psbt)?;
        let request = if send {
            if !summary.complete {
                return Err("This PSBT is not fully signed yet.".into());
            }
            Request::SendPsbt { summary, loaded }
        } else {
            if summary.complete {
                return Err("This PSBT is already fully signed. Broadcast it instead.".into());
            }
            if wallet::security(client, &name).map_err(|e| e.to_string())?
                == wallet::Security::WatchOnly
            {
                return Err("This wallet has no private keys to sign with.".into());
            }
            Request::SignPsbt { summary, loaded }
        };
        core.drafts
            .lock()
            .unwrap()
            .insert(request)
            .map_err(|e| e.to_string())
    })
    .await?;
    open_approval(&app).map_err(|e| e.to_string())?;
    Ok(id)
}

/// Saves a PSBT as a binary .psbt file, as Qt does, into the Downloads
/// folder under a new name, and returns where.
#[tauri::command]
async fn psbt_save(app: AppHandle, window: Window, psbt: String) -> Result<String, String> {
    require_window(&window, MAIN)?;
    let folder = app
        .path()
        .download_dir()
        .map_err(|e| format!("The Downloads folder was not found: {e}"))?;
    blocking(move || {
        let bytes = psbt::file_bytes(&psbt)?;
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_or(0, |d| d.as_secs() as i64);
        let path = folder.join(format!(
            "b3-hive-transaction-{}.psbt",
            history::utc(now).replace(':', "")
        ));
        let mut file = std::fs::File::create_new(&path)
            .map_err(|e| format!("Cannot create {}: {e}", path.display()))?;
        std::io::Write::write_all(&mut file, &bytes)
            .map_err(|e| format!("Cannot write {}: {e}", path.display()))?;
        Ok(path.display().to_string())
    })
    .await
}

/// Asks the approval window to release stakes back into ordinary B3.
#[tauri::command]
async fn release_draft(
    app: AppHandle,
    core: State<'_, Arc<Core>>,
    outpoints: Vec<Outpoint>,
) -> Result<String, String> {
    let core = Arc::clone(&core);
    let id = blocking(move || {
        let node = core.node();
        let (client, name) = page_wallet(&core, &node)?;
        let chain = node.chain().unwrap_or_default();
        let (summary, funded) = stake::prepare_release(client, &name, &chain, &outpoints)?;
        let inputs = funded.inputs.clone();
        core.drafts
            .lock()
            .unwrap()
            .insert(Request::Release { summary, funded })
            .map_err(|e| {
                wallet::release(client, &name, &inputs);
                e.to_string()
            })
    })
    .await?;
    open_approval(&app).map_err(|e| e.to_string())?;
    Ok(id)
}

/// Asks the approval window to bind (or rotate) the finality key.
#[tauri::command]
async fn finality_draft(app: AppHandle, core: State<'_, Arc<Core>>) -> Result<String, String> {
    let core = Arc::clone(&core);
    let id = blocking(move || {
        let node = core.node();
        let (client, name) = page_wallet(&core, &node)?;
        let status = stake::status(client, &name).map_err(|e| e.to_string())?;
        let Some(finality) = status.finality else {
            return Err("Create a stake first: it makes this wallet's validator key.".into());
        };
        let request = Request::BindFinality {
            chain: node.chain().unwrap_or_default(),
            wallet: name,
            rotate: finality.bound && !finality.revoked,
        };
        core.drafts
            .lock()
            .unwrap()
            .insert(request)
            .map_err(|e| e.to_string())
    })
    .await?;
    open_approval(&app).map_err(|e| e.to_string())?;
    Ok(id)
}

/// Starts staking with an unencrypted wallet. An encrypted wallet starts it
/// through the passphrase window instead.
#[tauri::command]
async fn stake_start(core: State<'_, Arc<Core>>) -> Result<(), String> {
    let core = Arc::clone(&core);
    blocking(move || {
        let node = core.node();
        let (client, name) = page_wallet(&core, &node)?;
        stake::start(client, &name, None)
    })
    .await
}

#[tauri::command]
async fn stake_stop(core: State<'_, Arc<Core>>) -> Result<(), String> {
    let core = Arc::clone(&core);
    blocking(move || {
        let node = core.node();
        let (client, name) = page_wallet(&core, &node)?;
        stake::stop(client, &name)
    })
    .await
}

/// The coins the page's wallet can spend, for coin control.
#[tauri::command]
async fn send_coins(core: State<'_, Arc<Core>>) -> Result<wallet::CoinList, String> {
    let core = Arc::clone(&core);
    blocking(move || {
        let node = core.node();
        let (client, name) = page_wallet(&core, &node)?;
        wallet::coin_list(client, &name).map_err(|e| e.to_string())
    })
    .await
}

/// The wallet's receiving addresses, newest last as the node lists them.
#[tauri::command]
async fn receive_addresses(core: State<'_, Arc<Core>>) -> Result<Vec<receive::Receiving>, String> {
    let core = Arc::clone(&core);
    blocking(move || {
        let node = core.node();
        let (client, name) = page_wallet(&core, &node)?;
        receive::addresses(client, &name).map_err(|e| e.to_string())
    })
    .await
}

/// The link and QR code for one of the wallet's receiving addresses.
#[tauri::command]
async fn receive_show(
    core: State<'_, Arc<Core>>,
    address: String,
) -> Result<receive::Request, String> {
    let core = Arc::clone(&core);
    blocking(move || {
        let node = core.node();
        let (client, name) = page_wallet(&core, &node)?;
        let chain = node.chain().unwrap_or_default();
        receive::show(client, &name, &chain, &address)
    })
    .await
}

// ---- wallets (main window) ----------------------------------------------------

/// Qt's wallet selector and File menu wallets.
#[derive(Serialize)]
pub struct WalletsView {
    #[serde(flatten)]
    pub wallets: wallet::Wallets,
    /// The wallet the pages use, if any is loaded.
    pub current: Option<String>,
    /// How the current wallet's keys are protected.
    pub security: Option<wallet::Security>,
}

fn wallets_view(core: &Core) -> Result<WalletsView, String> {
    let node = core.node();
    let client = node.client().ok_or("The node is not ready yet.")?;
    let current = match page_wallet(core, &node) {
        Ok((_, name)) => Some(name),
        Err(e) if e == NO_WALLET => None,
        Err(e) => return Err(e),
    };
    let wallets = wallet::wallets(client).map_err(|e| e.to_string())?;
    let security = match &current {
        Some(name) => Some(wallet::security(client, name).map_err(|e| e.to_string())?),
        None => None,
    };
    Ok(WalletsView {
        wallets,
        current,
        security,
    })
}

#[tauri::command]
async fn wallets(core: State<'_, Arc<Core>>) -> Result<WalletsView, String> {
    let core = Arc::clone(&core);
    blocking(move || wallets_view(&core)).await
}

/// The wallet actions of Qt's File menu and wallet selector.
#[derive(Debug, Deserialize)]
#[serde(tag = "action", content = "name", rename_all = "lowercase")]
pub enum WalletAction {
    /// Use a loaded wallet on the pages.
    Switch(String),
    Open(String),
    Close(String),
    Create(String),
}

/// Runs one wallet action and returns the new list. A wallet that was opened,
/// created or switched to becomes the pages' wallet.
#[tauri::command]
async fn wallet_manage(
    window: Window,
    core: State<'_, Arc<Core>>,
    action: WalletAction,
) -> Result<WalletsView, String> {
    require_window(&window, MAIN)?;
    let core = Arc::clone(&core);
    blocking(move || {
        let node = core.node();
        let client = node.client().ok_or("The node is not ready yet.")?;
        let pick = match action {
            WalletAction::Switch(name) => {
                let loaded = wallet::loaded(client).map_err(|e| e.to_string())?;
                if !loaded.contains(&name) {
                    return Err("That wallet is not loaded.".into());
                }
                Some(name)
            }
            WalletAction::Open(name) => {
                wallet::open(client, &name)?;
                Some(name)
            }
            WalletAction::Close(name) => {
                wallet::close(client, &name)?;
                None
            }
            WalletAction::Create(name) => Some(wallet::create(client, &name)?),
        };
        if let Some(name) = pick {
            *core.wallet.lock().unwrap() = Some(name);
        }
        wallets_view(&core)
    })
    .await
}

/// Asks for a file with the native picker, over the main window. None when
/// it is cancelled.
fn pick_file(app: &AppHandle, title: &str, filter: (&str, &[&str])) -> Option<std::path::PathBuf> {
    use tauri_plugin_dialog::DialogExt;
    let mut dialog = app
        .dialog()
        .file()
        .set_title(title)
        .add_filter(filter.0, filter.1)
        .add_filter("All files", &["*"]);
    if let Some(main) = app.get_webview_window(MAIN) {
        dialog = dialog.set_parent(&main);
    }
    dialog.blocking_pick_file()?.into_path().ok()
}

/// A wallet rescan's progress, for the Wallets page: None once the file is
/// chosen and the rescan has started, then the percentage while the node
/// reports one.
#[derive(Clone, Serialize)]
struct ScanProgress {
    wallet: String,
    percent: Option<u32>,
}

/// Sends wallet-scan events to the main window: one when it starts, then
/// the rescan's progress from getwalletinfo once a second, until the
/// returned guard is dropped. A wallet the node does not list yet (a
/// restore rescans before it does) just reports no percentage.
fn watch_scan(app: &AppHandle, core: &Arc<Core>, wallet: &str) -> ScanWatch {
    let done = Arc::new(AtomicBool::new(false));
    let (app, core, wallet, stop) = (
        app.clone(),
        Arc::clone(core),
        wallet.to_owned(),
        Arc::clone(&done),
    );
    let _ = app.emit_to(
        MAIN,
        "wallet-scan",
        ScanProgress {
            wallet: wallet.clone(),
            percent: None,
        },
    );
    thread::spawn(move || {
        while !stop.load(Ordering::SeqCst) {
            thread::sleep(Duration::from_secs(1));
            let node = core.node();
            let Some(client) = node.client() else {
                continue;
            };
            let Ok(info) = client.call_wallet(&wallet, "getwalletinfo", serde_json::json!([]))
            else {
                continue;
            };
            if stop.load(Ordering::SeqCst) {
                break;
            }
            if let Some(progress) = info["scanning"]["progress"].as_f64() {
                let _ = app.emit_to(
                    MAIN,
                    "wallet-scan",
                    ScanProgress {
                        wallet: wallet.clone(),
                        percent: Some((progress * 100.0).clamp(0.0, 100.0) as u32),
                    },
                );
            }
        }
    });
    ScanWatch(done)
}

/// Stops watch_scan when dropped.
struct ScanWatch(Arc<AtomicBool>);

impl Drop for ScanWatch {
    fn drop(&mut self) {
        self.0.store(true, Ordering::SeqCst);
    }
}

/// Qt's Restore wallet: a backup file, chosen with the native picker, is
/// loaded as the new wallet `name` and becomes the pages' wallet. None when
/// the picker is cancelled.
#[tauri::command]
async fn wallet_restore(
    app: AppHandle,
    window: Window,
    core: State<'_, Arc<Core>>,
    name: String,
) -> Result<Option<WalletsView>, String> {
    require_window(&window, MAIN)?;
    wallet::check_new_name(&name)?;
    let core = Arc::clone(&core);
    blocking(move || {
        let Some(path) = pick_file(
            &app,
            "Restore a wallet backup",
            ("Wallet backups", &["dat", "bak"]),
        ) else {
            return Ok(None);
        };
        let node = core.node();
        let client = node.patient_client().ok_or("The node is not ready yet.")?;
        let _watch = watch_scan(&app, &core, name.trim());
        let name = wallet::restore(client, &name, &path)?;
        *core.wallet.lock().unwrap() = Some(name);
        wallets_view(&core).map(Some)
    })
    .await
}

/// A legacy import's outcome for the Wallets page.
#[derive(Serialize)]
pub struct ImportView {
    imported: wallet::Imported,
    wallets: WalletsView,
}

/// Recovers a legacy B3 wallet from its dumpwallet file, chosen with the
/// native picker, into the new wallet `name`, which becomes the pages'
/// wallet. None when the picker is cancelled.
#[tauri::command]
async fn wallet_import_legacy(
    app: AppHandle,
    window: Window,
    core: State<'_, Arc<Core>>,
    name: String,
) -> Result<Option<ImportView>, String> {
    require_window(&window, MAIN)?;
    wallet::check_new_name(&name)?;
    let core = Arc::clone(&core);
    blocking(move || {
        let Some(path) = pick_file(
            &app,
            "Import a legacy wallet dump",
            ("Wallet dumps", &["txt", "dump"]),
        ) else {
            return Ok(None);
        };
        let node = core.node();
        let client = node.patient_client().ok_or("The node is not ready yet.")?;
        let _watch = watch_scan(&app, &core, name.trim());
        let imported = wallet::import_legacy(client, &name, &path)?;
        *core.wallet.lock().unwrap() = Some(imported.wallet.clone());
        Ok(Some(ImportView {
            imported,
            wallets: wallets_view(&core)?,
        }))
    })
    .await
}

/// Opens the passphrase window to encrypt the wallet in use or change its
/// passphrase. The outcome arrives as a passphrase-result event.
/// Async on purpose: see open_console.
#[tauri::command]
async fn passphrase_request(
    app: AppHandle,
    window: Window,
    core: State<'_, Arc<Core>>,
    kind: passphrase::Kind,
) -> Result<(), String> {
    require_window(&window, MAIN)?;
    if app.get_webview_window(PASSPHRASE).is_some() {
        return Err("The passphrase window is already open.".into());
    }
    let core2 = Arc::clone(&core);
    let wallet = blocking(move || {
        let node = core2.node();
        let (client, name) = page_wallet(&core2, &node)?;
        let security = wallet::security(client, &name).map_err(|e| e.to_string())?;
        passphrase::check_kind(kind, security)?;
        Ok(name)
    })
    .await?;
    let mut nonce = [0u8; 16];
    getrandom::fill(&mut nonce).map_err(|e| e.to_string())?;
    let id = nonce.iter().map(|b| format!("{b:02x}")).collect();
    *core.passphrase.lock().unwrap() = Some(passphrase::Request { id, kind, wallet });
    let (title, height) = match kind {
        passphrase::Kind::Encrypt => ("Encrypt wallet – B3 Hive", 520.0),
        passphrase::Kind::Change => ("Change passphrase – B3 Hive", 520.0),
        passphrase::Kind::Stake => ("Start staking – B3 Hive", 380.0),
    };
    WebviewWindowBuilder::new(&app, PASSPHRASE, WebviewUrl::App("passphrase.html".into()))
        .title(title)
        .inner_size(460.0, height)
        .resizable(false)
        .always_on_top(true)
        .focused(true)
        .incognito(true)
        .build()
        .map(|_| ())
        .map_err(|e| {
            core.passphrase.lock().unwrap().take();
            e.to_string()
        })
}

/// Qt's Lock wallet, for an encrypted wallet that is unlocked.
#[tauri::command]
async fn wallet_lock(window: Window, core: State<'_, Arc<Core>>) -> Result<(), String> {
    require_window(&window, MAIN)?;
    let core = Arc::clone(&core);
    blocking(move || {
        let node = core.node();
        let (client, name) = page_wallet(&core, &node)?;
        passphrase::lock(client, &name)
    })
    .await
}

/// Qt's Backup wallet, into the Downloads folder under a new name.
#[tauri::command]
async fn wallet_backup(
    app: AppHandle,
    window: Window,
    core: State<'_, Arc<Core>>,
) -> Result<String, String> {
    require_window(&window, MAIN)?;
    let folder = app
        .path()
        .download_dir()
        .map_err(|e| format!("The Downloads folder was not found: {e}"))?;
    let core = Arc::clone(&core);
    blocking(move || {
        let node = core.node();
        let (client, name) = page_wallet(&core, &node)?;
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_or(0, |d| d.as_secs() as i64);
        let label = if name.is_empty() {
            "default"
        } else {
            name.as_str()
        };
        let path = folder.join(format!(
            "b3-hive-wallet-{}-{}.dat",
            label.replace(' ', "_"),
            history::utc(now).replace(':', "")
        ));
        wallet::backup(client, &name, &path)?;
        Ok(path.display().to_string())
    })
    .await
}

/// The dashboard's chain, sync, network and mempool summary.
#[tauri::command]
async fn chain_overview(core: State<'_, Arc<Core>>) -> Result<chain::ChainOverview, String> {
    let core = Arc::clone(&core);
    blocking(move || {
        let node = core.node();
        let client = node.client().ok_or("The node is not ready yet.")?;
        chain::overview(client).map_err(|e| e.to_string())
    })
    .await
}

/// The network page's peer list.
#[tauri::command]
async fn network_peers(core: State<'_, Arc<Core>>) -> Result<Vec<chain::Peer>, String> {
    let core = Arc::clone(&core);
    blocking(move || {
        let node = core.node();
        let client = node.client().ok_or("The node is not ready yet.")?;
        chain::peers(client).map_err(|e| e.to_string())
    })
    .await
}

/// Funds the send, keeps it as a draft and opens the approval
/// window. Nothing is broadcast here; the page only learns the draft id.
#[tauri::command]
async fn draft_send(
    app: AppHandle,
    core: State<'_, Arc<Core>>,
    payment: wallet::Payment,
) -> Result<String, String> {
    let core = Arc::clone(&core);
    let id = blocking(move || {
        let node = core.node();
        let (client, name) = page_wallet(&core, &node)?;
        let chain = node.chain().unwrap_or_default();
        let (summary, funded) = wallet::prepare_payment(client, &name, &chain, &payment)?;
        let inputs = funded.inputs.clone();
        core.drafts
            .lock()
            .unwrap()
            .insert(Request::Pay { summary, funded })
            .map_err(|e| {
                wallet::release(client, &name, &inputs);
                e.to_string()
            })
    })
    .await?;
    open_approval(&app).map_err(|e| e.to_string())?;
    Ok(id)
}

/// Async on purpose: a window built inside a synchronous command deadlocks on
/// Windows (WebView2) and stays blank.
#[tauri::command]
async fn open_console(app: AppHandle) -> Result<(), String> {
    if let Some(window) = app.get_webview_window(CONSOLE) {
        return window.set_focus().map_err(|e| e.to_string());
    }
    WebviewWindowBuilder::new(&app, CONSOLE, WebviewUrl::App("console.html".into()))
        .title("Console – B3 Hive")
        .inner_size(860.0, 600.0)
        .incognito(true)
        .build()
        .map(|_| ())
        .map_err(|e| e.to_string())
}

/// The theme picked in the main window, or None to follow the system.
#[tauri::command]
fn ui_theme(core: State<'_, Arc<Core>>) -> Option<Theme> {
    *core.theme.lock().unwrap()
}

#[tauri::command]
fn set_ui_theme(window: Window, core: State<'_, Arc<Core>>, theme: Theme) -> Result<(), String> {
    require_window(&window, MAIN)?;
    *core.theme.lock().unwrap() = Some(theme);
    Ok(())
}

// ---- node settings (main window) --------------------------------------------

#[derive(Serialize)]
pub struct SettingsView {
    pub settings: settings::NodeSettings,
    /// The settings file, for the page to name.
    pub file: String,
    pub network: Network,
}

#[tauri::command]
fn node_settings(window: Window, core: State<'_, Arc<Core>>) -> Result<SettingsView, String> {
    require_window(&window, MAIN)?;
    let chain_dir = core.config.network.chain_dir(&core.config.datadir);
    Ok(SettingsView {
        settings: settings::read(&chain_dir)?,
        file: settings::path(&chain_dir).display().to_string(),
        network: core.config.network,
    })
}

/// Saves the node settings and restarts b3coind so they take effect. Nothing
/// is stopped when the settings or the file cannot be used.
#[tauri::command]
async fn node_settings_apply(
    app: AppHandle,
    window: Window,
    core: State<'_, Arc<Core>>,
    settings: settings::NodeSettings,
) -> Result<(), String> {
    require_window(&window, MAIN)?;
    let core = Arc::clone(&core);
    blocking(move || {
        let settings = settings.validated()?;
        let chain_dir = core.config.network.chain_dir(&core.config.datadir);
        settings::check(&chain_dir)?;
        let mut current = core.node.lock().unwrap();
        if core.exiting.load(Ordering::SeqCst) {
            return Err("B3 Hive is closing.".into());
        }
        if matches!(current.status(), NodeStatus::Setup) || core.setup.installing() {
            return Err("Finish setting up the node first.".into());
        }
        let _ = app.emit_to(MAIN, "node-status", NodeStatus::Stopping);
        current.stop(STOP_TIMEOUT);
        let written = settings::write(&chain_dir, &settings);
        // Started again even when the write failed, with the settings it had.
        if !core.exiting.load(Ordering::SeqCst) {
            *current = start_node(&app, &core.config);
        }
        written
    })
    .await
}

// ---- setup wizard (main window) ---------------------------------------------

#[derive(Serialize)]
pub struct SetupInfo {
    /// Where the chain will live.
    pub datadir: String,
    pub network: Network,
    /// The explorer's offers, newest first; empty when it could not be read.
    pub offers: Vec<bootstrap::Offer>,
    /// Why the offers are missing, for the page to show.
    pub offers_error: Option<String>,
}

fn require_setup(core: &Core) -> Result<(), String> {
    if !matches!(core.node().status(), NodeStatus::Setup) {
        return Err("The node is already set up.".into());
    }
    if core.setup.installing() {
        return Err("A bootstrap is being installed.".into());
    }
    Ok(())
}

/// The data folder and the explorer's current bootstraps.
#[tauri::command]
async fn setup_info(window: Window, core: State<'_, Arc<Core>>) -> Result<SetupInfo, String> {
    require_window(&window, MAIN)?;
    let core = Arc::clone(&core);
    blocking(move || {
        let (offers, offers_error) = match bootstrap::Explorer::new().offers() {
            Ok(offers) => (offers, None),
            Err(e) => (Vec::new(), Some(e)),
        };
        *core.setup.offers.lock().unwrap() = offers.clone();
        Ok(SetupInfo {
            datadir: core.config.datadir.display().to_string(),
            network: core.config.network,
            offers,
            offers_error,
        })
    })
    .await
}

/// Installs the explorer's bootstrap at `height`, then starts the node.
/// The page names only the height; the address, size and SHA-256 come from
/// the manifest the core itself read. Progress goes to the main window as
/// bootstrap-progress events, and a failure as bootstrap-failed.
#[tauri::command]
fn setup_bootstrap(
    app: AppHandle,
    window: Window,
    core: State<'_, Arc<Core>>,
    height: u64,
) -> Result<(), String> {
    require_window(&window, MAIN)?;
    require_setup(&core)?;
    if core.config.network != Network::Main {
        return Err("Bootstraps hold the mainnet chain; this node runs regtest.".into());
    }
    let offer = core
        .setup
        .offers
        .lock()
        .unwrap()
        .iter()
        .find(|o| o.height == height)
        .cloned()
        .ok_or("That bootstrap is no longer offered. Reload the list.")?;
    core.setup.cancel.store(false, Ordering::SeqCst);
    let worker = Arc::clone(&core);
    let handle = thread::spawn(move || {
        let chain_dir = worker.config.network.chain_dir(&worker.config.datadir);
        let mut last = None;
        let mut progress = |p: bootstrap::Progress| {
            // At most one event per 1% of the archive or per phase.
            let step = (offer.size / 100).max(1);
            let key = match &p {
                bootstrap::Progress::Downloading { received, .. } => (0, received / step),
                bootstrap::Progress::Verifying => (1, 0),
                bootstrap::Progress::Unpacking { written } => (2, written / (step * 2)),
                bootstrap::Progress::Done { .. } => (3, 0),
            };
            if last != Some(key) {
                last = Some(key);
                let _ = app.emit_to(MAIN, "bootstrap-progress", &p);
            }
        };
        let result = bootstrap::Explorer::new()
            .open(&offer)
            .and_then(|mut source| {
                bootstrap::install(
                    &mut source,
                    &offer,
                    &chain_dir,
                    &mut progress,
                    &worker.setup.cancel,
                )
            });
        match result {
            Ok(()) if !worker.exiting.load(Ordering::SeqCst) => {
                *worker.node.lock().unwrap() = start_node(&app, &worker.config);
            }
            Ok(()) => {}
            Err(error) => {
                let _ = app.emit_to(MAIN, "bootstrap-failed", error);
            }
        }
    });
    *core.setup.install.lock().unwrap() = Some(handle);
    Ok(())
}

#[tauri::command]
fn setup_cancel(window: Window, core: State<'_, Arc<Core>>) -> Result<(), String> {
    require_window(&window, MAIN)?;
    core.setup.cancel.store(true, Ordering::SeqCst);
    Ok(())
}

/// Starts the node to sync the chain from its peers.
#[tauri::command]
fn setup_sync(app: AppHandle, window: Window, core: State<'_, Arc<Core>>) -> Result<(), String> {
    require_window(&window, MAIN)?;
    require_setup(&core)?;
    let mut current = core.node.lock().unwrap();
    if core.exiting.load(Ordering::SeqCst) {
        return Err("B3 Hive is closing.".into());
    }
    *current = start_node(&app, &core.config);
    Ok(())
}

// ---- updates (main window) -------------------------------------------------

/// The update helper, started on first use and again if it stops. One
/// command runs at a time; a status read meanwhile gets the last result.
pub struct Updates {
    helper: PathBuf,
    args: Vec<String>,
    fetcher: HttpsFetcher,
    running: Mutex<Option<Updater>>,
    last: Mutex<Option<UpdateStatus>>,
}

/// Download progress goes to the main window at most every 256 KiB.
const PROGRESS_STEP: u64 = 256 * 1024;

impl Updates {
    fn new(helper: PathBuf, args: Vec<String>) -> Self {
        Self {
            helper,
            args,
            fetcher: HttpsFetcher::new(),
            running: Mutex::new(None),
            last: Mutex::new(None),
        }
    }

    fn run(
        &self,
        command: impl FnOnce(&mut Updater, &HttpsFetcher) -> Result<UpdateStatus, String>,
    ) -> Result<UpdateStatus, String> {
        let running = self
            .running
            .try_lock()
            .map_err(|_| "An update check or download is already running.".to_owned())?;
        self.run_on(running, command)
    }

    fn status(&self) -> Result<UpdateStatus, String> {
        match self.running.try_lock() {
            Ok(running) => self.run_on(running, |u, _| u.status()),
            Err(_) => self
                .last
                .lock()
                .unwrap()
                .clone()
                .ok_or_else(|| "The update helper is busy.".to_owned()),
        }
    }

    fn run_on(
        &self,
        mut running: MutexGuard<'_, Option<Updater>>,
        command: impl FnOnce(&mut Updater, &HttpsFetcher) -> Result<UpdateStatus, String>,
    ) -> Result<UpdateStatus, String> {
        if !running.as_mut().is_some_and(Updater::alive) {
            *running = Some(Updater::start(&self.helper, &self.args)?);
        }
        let result = command(running.as_mut().unwrap(), &self.fetcher);
        match &result {
            Ok(status) => *self.last.lock().unwrap() = Some(status.clone()),
            // The helper is in an unknown state: the next command starts a new one.
            Err(_) => *running = None,
        }
        result
    }

    /// Ends the helper unless a command is still using it; a helper left
    /// running ends by itself when the app's end of its pipe closes.
    fn stop(&self) {
        if let Ok(mut running) = self.running.try_lock() {
            running.take();
        }
    }
}

#[tauri::command]
async fn update_status(window: Window, core: State<'_, Arc<Core>>) -> Result<UpdateStatus, String> {
    require_window(&window, MAIN)?;
    let core = Arc::clone(&core);
    blocking(move || core.updates.status()).await
}

/// Fetches and verifies the signed manifest.
#[tauri::command]
async fn update_check(window: Window, core: State<'_, Arc<Core>>) -> Result<UpdateStatus, String> {
    require_window(&window, MAIN)?;
    let core = Arc::clone(&core);
    blocking(move || core.updates.run(|u, net| u.check(net))).await
}

/// Downloads and verifies the offered release. Installing stays manual.
#[tauri::command]
async fn update_download(
    app: AppHandle,
    window: Window,
    core: State<'_, Arc<Core>>,
) -> Result<UpdateStatus, String> {
    require_window(&window, MAIN)?;
    let core = Arc::clone(&core);
    blocking(move || {
        let mut reported = 0u64;
        core.updates.run(|u, net| {
            u.download(net, &mut |received| {
                if received - reported >= PROGRESS_STEP {
                    reported = received;
                    let _ = app.emit_to(MAIN, "update-progress", received);
                }
            })
        })
    })
    .await
}

// ---- approval window commands ----------------------------------------------

/// The draft as the approval window sees it: whether approving needs the
/// wallet's passphrase is checked now, since the wallet may have been
/// locked or unlocked since the draft was made.
fn draft_view(client: Option<&rpc::Client>, draft: drafts::Draft) -> DraftView {
    let locked = draft.request.needs_keys()
        && client.is_some_and(|c| {
            wallet::security(c, draft.request.wallet()).ok() == Some(wallet::Security::Locked)
        });
    DraftView {
        id: draft.id,
        request: draft.request,
        locked,
    }
}

/// What an approved request did.
enum Done {
    Sent(String),
    Signed { psbt: String, complete: bool },
}

/// Carries out an approved request.
fn execute(client: &rpc::Client, request: Request) -> Result<Done, String> {
    match request {
        Request::SignPsbt { summary, loaded } => psbt::sign(client, &summary.wallet, &loaded)
            .map(|(psbt, complete)| Done::Signed { psbt, complete }),
        Request::SendPsbt { summary, loaded } => {
            psbt::send(client, &summary, &loaded).map(Done::Sent)
        }
        request => execute_sent(client, request).map(Done::Sent),
    }
}

/// Carries out an approved request that ends in a transaction id.
fn execute_sent(client: &rpc::Client, request: Request) -> Result<String, String> {
    match request {
        Request::Pay { summary, funded } | Request::Release { summary, funded } => {
            let sent = wallet::sign(client, &summary, &funded)
                .and_then(|hex| wallet::broadcast(client, &hex).map_err(|e| e.to_string()));
            match &sent {
                Ok(_) => wallet::save_labels(client, &summary.wallet, &funded.labels),
                Err(_) => wallet::release(client, &summary.wallet, &funded.inputs),
            }
            sent
        }
        Request::Stake { wallet, amount, .. } => stake::create(client, &wallet, &amount),
        Request::BindFinality { wallet, .. } => stake::bind(client, &wallet),
        Request::SignPsbt { .. } | Request::SendPsbt { .. } => unreachable!("handled by execute"),
    }
}

fn oldest_view(core: &Core) -> Option<DraftView> {
    let draft = core.drafts.lock().unwrap().oldest()?;
    let node = core.node();
    Some(draft_view(node.client(), draft))
}

#[tauri::command]
async fn approval_pending(
    window: Window,
    core: State<'_, Arc<Core>>,
) -> Result<Option<DraftView>, String> {
    require_window(&window, APPROVAL)?;
    let core = Arc::clone(&core);
    blocking(move || Ok(oldest_view(&core))).await
}

/// What happened to an approved draft once it was taken.
type Outcome = Result<Done, String>;

/// Approve carries the draft out: a payment is signed, checked against what
/// was approved and submitted; a staking request goes to the node. Reject
/// frees a payment's coins. Either way the draft is
/// consumed, so it can never be submitted twice. A locked wallet is unlocked
/// with `passphrase` only to sign and is locked again straight after; a
/// missing or wrong passphrase leaves the draft waiting, to try again.
/// Returns the next waiting draft; the window closes when there is none.
#[tauri::command]
async fn approval_decide(
    app: AppHandle,
    window: Window,
    core: State<'_, Arc<Core>>,
    id: String,
    approve: bool,
    passphrase: Option<String>,
) -> Result<Option<DraftView>, String> {
    require_window(&window, APPROVAL)?;
    let core = Arc::clone(&core);
    let result = if approve {
        let core2 = Arc::clone(&core);
        let id2 = id.clone();
        let outcome: Outcome = blocking(move || {
            let node = core2.node();
            let client = node.client().ok_or("The node is not ready.")?;
            let waiting = core2
                .drafts
                .lock()
                .unwrap()
                .get(&id2)
                .ok_or("This request was already decided.")?;
            let wallet_name = waiting.request.wallet().to_owned();
            let locked = waiting.request.needs_keys()
                && wallet::security(client, &wallet_name).map_err(|e| e.to_string())?
                    == wallet::Security::Locked;
            if locked {
                wallet::unlock_for_signing(
                    client,
                    &wallet_name,
                    passphrase.as_deref().unwrap_or(""),
                )?;
            }
            // From here on the draft is consumed, whatever happens.
            let taken = core2.drafts.lock().unwrap().take(&id2);
            let outcome = match taken {
                None => Err("This request was already decided.".to_owned()),
                Some(draft) => execute(client, draft.request),
            };
            if locked {
                // Only a lock this approval opened is closed again.
                let _ = passphrase::lock(client, &wallet_name);
            }
            Ok(outcome)
        })
        .await?;
        match outcome {
            Ok(Done::Sent(txid)) => DraftResult::Sent {
                id: id.clone(),
                txid,
            },
            Ok(Done::Signed { psbt, complete }) => DraftResult::Signed {
                id: id.clone(),
                psbt,
                complete,
            },
            Err(error) => DraftResult::Failed {
                id: id.clone(),
                error,
            },
        }
    } else {
        let draft = core
            .drafts
            .lock()
            .unwrap()
            .take(&id)
            .ok_or("This request was already decided.")?;
        let core2 = Arc::clone(&core);
        blocking(move || {
            if let Some(client) = core2.node().client() {
                wallet::release(client, draft.request.wallet(), draft.request.reserved());
            }
            Ok(())
        })
        .await?;
        DraftResult::Rejected { id: id.clone() }
    };
    let _ = app.emit_to(MAIN, "draft-result", result);
    let core2 = Arc::clone(&core);
    let next = blocking(move || Ok(oldest_view(&core2))).await?;
    if next.is_none() {
        let _ = window.close();
    }
    Ok(next)
}

// ---- passphrase window commands --------------------------------------------

#[tauri::command]
fn passphrase_pending(
    window: Window,
    core: State<'_, Arc<Core>>,
) -> Result<Option<passphrase::Request>, String> {
    require_window(&window, PASSPHRASE)?;
    Ok(core.passphrase.lock().unwrap().clone())
}

/// Runs the request with the typed passphrases. A mistake (wrong current
/// passphrase, mismatch) leaves the window open to try again; success tells
/// the main window and closes this one.
#[tauri::command]
async fn passphrase_submit(
    app: AppHandle,
    window: Window,
    core: State<'_, Arc<Core>>,
    id: String,
    entry: passphrase::Entry,
) -> Result<(), String> {
    require_window(&window, PASSPHRASE)?;
    let request = core
        .passphrase
        .lock()
        .unwrap()
        .clone()
        .filter(|r| r.id == id)
        .ok_or("This request is no longer open.")?;
    let core2 = Arc::clone(&core);
    let done = request.clone();
    blocking(move || {
        let node = core2.node();
        let client = node.client().ok_or("The node is not ready.")?;
        passphrase::apply(client, &done, &entry)
    })
    .await?;
    core.passphrase.lock().unwrap().take();
    let _ = app.emit_to(
        MAIN,
        "passphrase-result",
        PassResult::Done {
            kind: request.kind,
            wallet: request.wallet,
        },
    );
    let _ = window.close();
    Ok(())
}

/// Closes the passphrase window; closing it cancels the request.
#[tauri::command]
fn passphrase_cancel(window: Window) -> Result<(), String> {
    require_window(&window, PASSPHRASE)?;
    window.close().map_err(|e| e.to_string())
}

// ---- console window commands -----------------------------------------------

/// Parses and runs one typed line with the console's full-access login,
/// against `wallet` if one is chosen. Err means the line was refused before
/// anything ran. A successful `stop` closes the app, as in the Qt console.
#[tauri::command]
async fn console_exec(
    app: AppHandle,
    window: Window,
    core: State<'_, Arc<Core>>,
    line: String,
    wallet: Option<String>,
) -> Result<console::Reply, String> {
    require_window(&window, CONSOLE)?;
    let core = Arc::clone(&core);
    let stop = console::is_stop(&line);
    let reply = blocking(move || {
        let node = core.node();
        let client = node.console_client().ok_or("The node is not running.")?;
        let mut exec = |method: &str, params: serde_json::Value| {
            match &wallet {
                Some(name) => client.call_wallet(name, method, params),
                None => client.call(method, params),
            }
            .map_err(console_error)
        };
        console::submit(&line, &mut exec)
    })
    .await?;
    if stop && reply.ok {
        app.exit(0);
    }
    Ok(reply)
}

/// Words for the console's completion popup, from the node's `help`
/// overview. Err carries the node's reason when it cannot list commands.
#[tauri::command]
async fn console_completions(
    window: Window,
    core: State<'_, Arc<Core>>,
) -> Result<Vec<String>, String> {
    require_window(&window, CONSOLE)?;
    let core = Arc::clone(&core);
    blocking(move || {
        let node = core.node();
        let client = node.console_client().ok_or("The node is not running.")?;
        let help = client
            .call("help", serde_json::json!([]))
            .map_err(|e| e.to_string())?;
        Ok(console::completion_words(help.as_str().unwrap_or_default()))
    })
    .await
}

/// The loaded wallets, for the console's wallet selector.
#[tauri::command]
async fn console_wallets(
    window: Window,
    core: State<'_, Arc<Core>>,
) -> Result<Vec<String>, String> {
    require_window(&window, CONSOLE)?;
    let core = Arc::clone(&core);
    blocking(move || {
        let node = core.node();
        let client = node.console_client().ok_or("The node is not running.")?;
        let wallets = client
            .call("listwallets", serde_json::json!([]))
            .map_err(|e| e.to_string())?;
        Ok(wallets
            .as_array()
            .map(|w| {
                w.iter()
                    .filter_map(|n| n.as_str().map(str::to_owned))
                    .collect()
            })
            .unwrap_or_default())
    })
    .await
}

fn console_error(e: RpcError) -> console::ConsoleError {
    match e {
        RpcError::Rpc { code, message } => console::ConsoleError::Rpc { code, message },
        other => console::ConsoleError::Error(other.to_string()),
    }
}

/// Capabilities already confine each window's commands to that window; this
/// check keeps that true even if a capability file is edited by mistake.
fn require_window(window: &Window, label: &str) -> Result<(), String> {
    if window.label() == label {
        Ok(())
    } else {
        Err(format!("Only the {label} window can do this."))
    }
}

fn open_approval(app: &AppHandle) -> tauri::Result<()> {
    if let Some(window) = app.get_webview_window(APPROVAL) {
        // Already open: it shows the oldest waiting draft.
        return window.set_focus();
    }
    WebviewWindowBuilder::new(app, APPROVAL, WebviewUrl::App("approval.html".into()))
        .title("Approve – B3 Hive")
        .inner_size(480.0, 560.0)
        .resizable(false)
        .always_on_top(true)
        .focused(true)
        .incognito(true)
        .build()?;
    Ok(())
}

/// Runs blocking RPC work off the async runtime's threads.
async fn blocking<T: Send + 'static>(
    f: impl FnOnce() -> Result<T, String> + Send + 'static,
) -> Result<T, String> {
    tauri::async_runtime::spawn_blocking(f)
        .await
        .map_err(|e| e.to_string())?
}

// ---- startup ----------------------------------------------------------------

/// The node is the b3coind packaged next to the app's own executable (a
/// Tauri sidecar), unless B3_HIVE_B3COIND names another one for development.
/// It runs B3 mainnet in the legacy client's data folder, so a Qt user finds
/// their chain and wallets. B3_HIVE_NETWORK=regtest is for development: a
/// peerless regtest node in the app's own data directory. B3_HIVE_DATADIR
/// moves the data folder for either. B3_HIVE_REGTEST_ARGS replaces the
/// peerless defaults, to join a local test network such as a FlowMesh test
/// market; mainnet refuses it.
fn node_config(app: &tauri::App) -> Result<NodeConfig, Box<dyn std::error::Error>> {
    let b3coind = match std::env::var_os("B3_HIVE_B3COIND") {
        Some(path) => PathBuf::from(path),
        None => packaged_sidecar("b3-hive-node")?,
    };
    let network = Network::from_setting(std::env::var("B3_HIVE_NETWORK").ok().as_deref())?;
    let datadir = match (std::env::var_os("B3_HIVE_DATADIR"), network) {
        (Some(dir), _) => PathBuf::from(dir),
        (None, Network::Regtest) => app.path().app_data_dir()?.join("spike-regtest"),
        (None, Network::Main) => legacy_datadir(
            std::env::consts::OS,
            &app.path().home_dir()?,
            &app.path().data_dir()?,
        ),
    };
    let regtest_args: Vec<String> = std::env::var("B3_HIVE_REGTEST_ARGS")
        .map(|extra| extra.split_whitespace().map(str::to_owned).collect())
        .unwrap_or_default();
    if !regtest_args.is_empty() && network != Network::Regtest {
        return Err("B3_HIVE_REGTEST_ARGS needs B3_HIVE_NETWORK=regtest".into());
    }
    Ok(NodeConfig {
        b3coind,
        datadir,
        network,
        regtest_args,
    })
}

/// The legacy B3-CoinV2 client's data folder, which b3coind also defaults
/// to: %APPDATA%\B3-CoinV2, ~/Library/Application Support/B3-CoinV2, or
/// ~/.B3-CoinV2. `data` is the platform's roaming application data folder.
fn legacy_datadir(os: &str, home: &std::path::Path, data: &std::path::Path) -> PathBuf {
    match os {
        "windows" | "macos" => data.join("B3-CoinV2"),
        _ => home.join(".B3-CoinV2"),
    }
}

/// The update helper is packaged next to the app like b3coind, unless
/// B3_HIVE_UPDATE names another one. B3_HIVE_UPDATE_ARGS adds arguments for
/// development, such as a test channel, which only unpinned developer
/// builds of the helper accept. Its state lives in the app's local data.
fn updates(app: &tauri::App) -> Result<Updates, Box<dyn std::error::Error>> {
    let helper = match std::env::var_os("B3_HIVE_UPDATE") {
        Some(path) => PathBuf::from(path),
        None => packaged_sidecar("b3-hive-update")?,
    };
    let state = app.path().app_local_data_dir()?.join("updates");
    let mut args = vec![format!("--state-dir={}", state.display())];
    if let Ok(extra) = std::env::var("B3_HIVE_UPDATE_ARGS") {
        args.extend(extra.split_whitespace().map(str::to_owned));
    }
    Ok(Updates::new(helper, args))
}

/// Bundles place sidecars beside the main executable, with the target triple
/// stripped from the name: /usr/bin on Linux packages, the install folder on
/// Windows, Contents/MacOS on macOS. The bundled b3coind is named b3-hive-node
/// so it cannot clash with a b3coind installed on its own in /usr/bin. If it
/// is missing, the main window shows the start failure with this path.
fn packaged_sidecar(name: &str) -> Result<PathBuf, Box<dyn std::error::Error>> {
    let exe = std::env::current_exe()?;
    let dir = exe.parent().ok_or("the app executable has no folder")?;
    Ok(dir.join(format!("{name}{}", std::env::consts::EXE_SUFFIX)))
}

/// Starts b3coind and follows it on a thread of its own: init, then the tip,
/// until it exits. Every status change goes to the main window.
fn start_node(app: &AppHandle, config: &NodeConfig) -> Arc<Node> {
    let node = Node::start(config);
    let follower = Arc::clone(&node);
    let handle = app.clone();
    thread::spawn(move || {
        let emit = |status: &NodeStatus| {
            let _ = handle.emit_to(MAIN, "node-status", status);
        };
        emit(&follower.status());
        if let NodeStatus::Ready { .. } = follower.wait_ready(START_TIMEOUT, emit) {
            follower.follow(FOLLOW_INTERVAL, emit);
        }
    });
    node
}

pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .setup(|app| {
            let config = node_config(app)?;
            let updates = updates(app)?;
            // A mainnet folder without a chain waits for the setup wizard.
            // B3_HIVE_SETUP=1 shows the wizard on regtest too, for development.
            let fresh = bootstrap::is_fresh(&config.network.chain_dir(&config.datadir));
            let wizard = match config.network {
                Network::Main => fresh,
                Network::Regtest => fresh && std::env::var("B3_HIVE_SETUP").as_deref() == Ok("1"),
            };
            let node = if wizard {
                Node::waiting_for_setup(&config)
            } else {
                start_node(app.handle(), &config)
            };
            app.manage(Arc::new(Core {
                node: Mutex::new(node),
                config,
                exiting: AtomicBool::new(false),
                drafts: Mutex::new(DraftStore::default()),
                wallet: Mutex::new(None),
                passphrase: Mutex::new(None),
                theme: Mutex::new(None),
                updates,
                setup: Setup::default(),
            }));
            Ok(())
        })
        .on_window_event(|window, event| {
            // Closing the approval window without deciding rejects what waits.
            if window.label() == APPROVAL && matches!(event, WindowEvent::Destroyed) {
                let core = Arc::clone(&window.state::<Arc<Core>>());
                let drafts = core.drafts.lock().unwrap().take_all();
                for draft in &drafts {
                    let _ = window.emit_to(
                        MAIN,
                        "draft-result",
                        DraftResult::Rejected {
                            id: draft.id.clone(),
                        },
                    );
                }
                // Their reserved coins are freed off the event loop.
                thread::spawn(move || {
                    if let Some(client) = core.node().client() {
                        for d in drafts {
                            wallet::release(client, d.request.wallet(), d.request.reserved());
                        }
                    }
                });
            }
            // Closing the passphrase window without finishing cancels it.
            if window.label() == PASSPHRASE && matches!(event, WindowEvent::Destroyed) {
                let core = window.state::<Arc<Core>>();
                let pending = core.passphrase.lock().unwrap().take();
                if let Some(r) = pending {
                    let _ = window.emit_to(
                        MAIN,
                        "passphrase-result",
                        PassResult::Cancelled { kind: r.kind },
                    );
                }
            }
            // The main window is the app: closing it ends the app (and the
            // node) even while the console or approval window is open.
            if window.label() == MAIN && matches!(event, WindowEvent::Destroyed) {
                window.app_handle().exit(0);
            }
        })
        .invoke_handler(tauri::generate_handler![
            node_status,
            restart_node,
            wallet_overview,
            wallet_activity,
            chain_overview,
            network_peers,
            dev_fund,
            draft_send,
            send_coins,
            book_list,
            book_save,
            stake_status,
            trade_markets,
            trade_market,
            stake_draft,
            release_draft,
            wallet_restore,
            wallet_import_legacy,
            psbt_inspect,
            psbt_draft,
            psbt_save,
            finality_draft,
            stake_start,
            stake_stop,
            open_console,
            ui_theme,
            set_ui_theme,
            update_status,
            update_check,
            update_download,
            node_info,
            network_totals,
            wallets,
            wallet_manage,
            passphrase_request,
            wallet_lock,
            passphrase_pending,
            passphrase_submit,
            passphrase_cancel,
            receive_new,
            receive_addresses,
            receive_show,
            wallet_backup,
            wallet_history,
            wallet_transaction,
            wallet_export_csv,
            node_settings,
            node_settings_apply,
            setup_info,
            setup_bootstrap,
            setup_cancel,
            setup_sync,
            approval_pending,
            approval_decide,
            console_exec,
            console_completions,
            console_wallets
        ])
        .build(tauri::generate_context!())
        .expect("failed to build the B3 Hive app")
        .run(|app, event| {
            if let RunEvent::Exit = event {
                if let Some(core) = app.try_state::<Arc<Core>>() {
                    core.exiting.store(true, Ordering::SeqCst);
                    core.setup.stop();
                    let node = core.node.lock().unwrap();
                    node.stop(STOP_TIMEOUT);
                    core.updates.stop();
                }
            }
        });
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::Path;

    #[test]
    fn mainnet_uses_the_legacy_data_folder() {
        let home = Path::new("/home/u");
        assert_eq!(
            legacy_datadir("linux", home, Path::new("/home/u/.local/share")),
            Path::new("/home/u/.B3-CoinV2")
        );
        assert_eq!(
            legacy_datadir(
                "macos",
                Path::new("/Users/u"),
                Path::new("/Users/u/Library/Application Support")
            ),
            Path::new("/Users/u/Library/Application Support/B3-CoinV2")
        );
        assert_eq!(
            legacy_datadir(
                "windows",
                Path::new("C:/Users/u"),
                Path::new("C:/Users/u/AppData/Roaming")
            ),
            Path::new("C:/Users/u/AppData/Roaming/B3-CoinV2")
        );
    }
}
