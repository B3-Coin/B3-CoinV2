//! The trusted core of the B3 Hive app. Windows only ask; the core holds the
//! node credentials, builds every transaction itself, and submits one only
//! after the approval window approves it.

pub mod drafts;
pub mod node;
pub mod rpc;
pub mod wallet;

use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use serde::Serialize;
use tauri::{AppHandle, Emitter, Manager, RunEvent, State, WebviewUrl, WebviewWindowBuilder, Window, WindowEvent};

use drafts::{DraftStore, DraftView};
use node::{Node, NodeConfig, NodeStatus};

const START_TIMEOUT: Duration = Duration::from_secs(600);
const STOP_TIMEOUT: Duration = Duration::from_secs(120);
const MAIN: &str = "main";
const APPROVAL: &str = "approval";

pub struct Core {
    node: Arc<Node>,
    drafts: Mutex<DraftStore>,
}

#[derive(Clone, Serialize)]
pub struct WalletOverview {
    pub wallet: String,
    pub spendable: String,
    pub immature: String,
    /// The funding button is offered on regtest only.
    pub can_fund: bool,
}

/// Sent to the main window when a draft is decided.
#[derive(Clone, Serialize)]
#[serde(tag = "outcome", rename_all = "lowercase")]
pub enum DraftResult {
    Sent { id: String, txid: String },
    Rejected { id: String },
    Failed { id: String, error: String },
}

// ---- main window commands --------------------------------------------------

#[tauri::command]
fn node_status(core: State<'_, Arc<Core>>) -> NodeStatus {
    core.node.status()
}

#[tauri::command]
async fn wallet_overview(core: State<'_, Arc<Core>>) -> Result<WalletOverview, String> {
    let core = Arc::clone(&core);
    blocking(move || {
        let client = core.node.client().ok_or("The node is not ready yet.")?;
        wallet::ensure_wallet(client).map_err(|e| e.to_string())?;
        let (spendable, immature) = wallet::balances(client).map_err(|e| e.to_string())?;
        Ok(WalletOverview {
            wallet: wallet::SPIKE_WALLET.into(),
            spendable,
            immature,
            can_fund: core.node.chain().as_deref() == Some("regtest"),
        })
    })
    .await
}

#[tauri::command]
async fn dev_fund(core: State<'_, Arc<Core>>) -> Result<(), String> {
    let core = Arc::clone(&core);
    blocking(move || {
        if core.node.chain().as_deref() != Some("regtest") {
            return Err("Funding by mining is only available on regtest.".into());
        }
        let client = core.node.client().ok_or("The node is not ready yet.")?;
        wallet::ensure_wallet(client).map_err(|e| e.to_string())?;
        wallet::fund_regtest(client).map_err(|e| e.to_string())
    })
    .await
}

/// Builds and signs the send, keeps it as a draft and opens the approval
/// window. Nothing is broadcast here; the page only learns the draft id.
#[tauri::command]
async fn draft_send(app: AppHandle, core: State<'_, Arc<Core>>, address: String, amount: String) -> Result<String, String> {
    let core = Arc::clone(&core);
    let id = blocking(move || {
        let client = core.node.client().ok_or("The node is not ready yet.")?;
        let chain = core.node.chain().unwrap_or_default();
        wallet::ensure_wallet(client).map_err(|e| e.to_string())?;
        let (summary, hex) = wallet::prepare_send(client, &chain, &address, &amount)?;
        core.drafts.lock().unwrap().insert(summary, hex).map_err(|e| e.to_string())
    })
    .await?;
    open_approval(&app).map_err(|e| e.to_string())?;
    Ok(id)
}

// ---- approval window commands ----------------------------------------------

#[tauri::command]
fn approval_pending(window: Window, core: State<'_, Arc<Core>>) -> Result<Option<DraftView>, String> {
    require_approval_window(&window)?;
    Ok(core.drafts.lock().unwrap().oldest())
}

/// Approve submits the draft's stored bytes exactly; reject discards them.
/// Either way the draft is consumed, so it can never be submitted twice.
/// Returns the next waiting draft; the window closes when there is none.
#[tauri::command]
async fn approval_decide(
    app: AppHandle,
    window: Window,
    core: State<'_, Arc<Core>>,
    id: String,
    approve: bool,
) -> Result<Option<DraftView>, String> {
    require_approval_window(&window)?;
    let core = Arc::clone(&core);
    let draft = core.drafts.lock().unwrap().take(&id).ok_or("This request was already decided.")?;
    let result = if approve {
        let core = Arc::clone(&core);
        blocking(move || {
            let client = core.node.client().ok_or("The node is not ready.")?;
            Ok(wallet::broadcast(client, &draft.hex))
        })
        .await?
        .map(|txid| DraftResult::Sent { id: id.clone(), txid })
        .unwrap_or_else(|e| DraftResult::Failed { id: id.clone(), error: e.to_string() })
    } else {
        DraftResult::Rejected { id: id.clone() }
    };
    let _ = app.emit_to(MAIN, "draft-result", result);
    let next = core.drafts.lock().unwrap().oldest();
    if next.is_none() {
        let _ = window.close();
    }
    Ok(next)
}

/// Capabilities already confine these commands to the approval window; this
/// check keeps that true even if a capability file is edited by mistake.
fn require_approval_window(window: &Window) -> Result<(), String> {
    if window.label() == APPROVAL {
        Ok(())
    } else {
        Err("Only the approval window can do this.".into())
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
async fn blocking<T: Send + 'static>(f: impl FnOnce() -> Result<T, String> + Send + 'static) -> Result<T, String> {
    tauri::async_runtime::spawn_blocking(f).await.map_err(|e| e.to_string())?
}

// ---- startup ----------------------------------------------------------------

/// Spike configuration: B3_HIVE_B3COIND names the b3coind binary (bundling it
/// as a sidecar comes with packaging); the regtest datadir defaults to the
/// app's data directory and can be moved with B3_HIVE_DATADIR.
fn node_config(app: &tauri::App) -> Result<NodeConfig, Box<dyn std::error::Error>> {
    let b3coind = std::env::var_os("B3_HIVE_B3COIND")
        .map(PathBuf::from)
        .ok_or("set B3_HIVE_B3COIND to the b3coind binary")?;
    let datadir = match std::env::var_os("B3_HIVE_DATADIR") {
        Some(dir) => PathBuf::from(dir),
        None => app.path().app_data_dir()?.join("spike-regtest"),
    };
    Ok(NodeConfig { b3coind, datadir })
}

pub fn run() {
    tauri::Builder::default()
        .setup(|app| {
            let config = node_config(app)?;
            let node = Node::start(&config);
            app.manage(Arc::new(Core { node: Arc::clone(&node), drafts: Mutex::new(DraftStore::default()) }));
            let handle = app.handle().clone();
            thread::spawn(move || {
                node.wait_ready(START_TIMEOUT, |status| {
                    let _ = handle.emit_to(MAIN, "node-status", status);
                });
            });
            Ok(())
        })
        .on_window_event(|window, event| {
            // Closing the approval window without deciding rejects what waits.
            if window.label() == APPROVAL && matches!(event, WindowEvent::Destroyed) {
                let core = window.state::<Arc<Core>>();
                for draft in core.drafts.lock().unwrap().take_all() {
                    let _ = window.emit_to(MAIN, "draft-result", DraftResult::Rejected { id: draft.id });
                }
            }
        })
        .invoke_handler(tauri::generate_handler![
            node_status,
            wallet_overview,
            dev_fund,
            draft_send,
            approval_pending,
            approval_decide
        ])
        .build(tauri::generate_context!())
        .expect("failed to build the B3 Hive app")
        .run(|app, event| {
            if let RunEvent::Exit = event {
                if let Some(core) = app.try_state::<Arc<Core>>() {
                    core.node.stop(STOP_TIMEOUT);
                }
            }
        });
}
