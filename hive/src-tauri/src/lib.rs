//! The trusted core of the B3 Hive app. Windows only ask; the core holds the
//! node credentials, builds every transaction itself, and submits one only
//! after the approval window approves it. The one exception is the console
//! window: commands typed there run with full access, as in the Qt console.

pub mod console;
pub mod drafts;
pub mod node;
pub mod rpc;
pub mod wallet;

use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use serde::Serialize;
use tauri::{
    AppHandle, Emitter, Manager, RunEvent, State, WebviewUrl, WebviewWindowBuilder, Window,
    WindowEvent,
};

use drafts::{DraftStore, DraftView};
use node::{Node, NodeConfig, NodeStatus};
use rpc::RpcError;

const START_TIMEOUT: Duration = Duration::from_secs(600);
const STOP_TIMEOUT: Duration = Duration::from_secs(120);
const MAIN: &str = "main";
const APPROVAL: &str = "approval";
const CONSOLE: &str = "console";

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
async fn draft_send(
    app: AppHandle,
    core: State<'_, Arc<Core>>,
    address: String,
    amount: String,
) -> Result<String, String> {
    let core = Arc::clone(&core);
    let id = blocking(move || {
        let client = core.node.client().ok_or("The node is not ready yet.")?;
        let chain = core.node.chain().unwrap_or_default();
        wallet::ensure_wallet(client).map_err(|e| e.to_string())?;
        let (summary, hex) = wallet::prepare_send(client, &chain, &address, &amount)?;
        core.drafts
            .lock()
            .unwrap()
            .insert(summary, hex)
            .map_err(|e| e.to_string())
    })
    .await?;
    open_approval(&app).map_err(|e| e.to_string())?;
    Ok(id)
}

#[tauri::command]
fn open_console(app: AppHandle) -> Result<(), String> {
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

// ---- approval window commands ----------------------------------------------

#[tauri::command]
fn approval_pending(
    window: Window,
    core: State<'_, Arc<Core>>,
) -> Result<Option<DraftView>, String> {
    require_window(&window, APPROVAL)?;
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
    require_window(&window, APPROVAL)?;
    let core = Arc::clone(&core);
    let draft = core
        .drafts
        .lock()
        .unwrap()
        .take(&id)
        .ok_or("This request was already decided.")?;
    let result = if approve {
        let core = Arc::clone(&core);
        blocking(move || {
            let client = core.node.client().ok_or("The node is not ready.")?;
            Ok(wallet::broadcast(client, &draft.hex))
        })
        .await?
        .map(|txid| DraftResult::Sent {
            id: id.clone(),
            txid,
        })
        .unwrap_or_else(|e| DraftResult::Failed {
            id: id.clone(),
            error: e.to_string(),
        })
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
        let client = core
            .node
            .console_client()
            .ok_or("The node is not running.")?;
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
        let client = core
            .node
            .console_client()
            .ok_or("The node is not running.")?;
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
        let client = core
            .node
            .console_client()
            .ok_or("The node is not running.")?;
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

/// Spike configuration: the node is the b3coind packaged next to the app's
/// own executable (a Tauri sidecar), unless B3_HIVE_B3COIND names another one
/// for development. The regtest datadir defaults to the app's data directory
/// and can be moved with B3_HIVE_DATADIR.
fn node_config(app: &tauri::App) -> Result<NodeConfig, Box<dyn std::error::Error>> {
    let b3coind = match std::env::var_os("B3_HIVE_B3COIND") {
        Some(path) => PathBuf::from(path),
        None => packaged_b3coind()?,
    };
    let datadir = match std::env::var_os("B3_HIVE_DATADIR") {
        Some(dir) => PathBuf::from(dir),
        None => app.path().app_data_dir()?.join("spike-regtest"),
    };
    Ok(NodeConfig { b3coind, datadir })
}

/// Bundles place sidecars beside the main executable, with the target triple
/// stripped from the name: /usr/bin on Linux packages, the install folder on
/// Windows, Contents/MacOS on macOS. If it is missing, the main window shows
/// the start failure with this path.
fn packaged_b3coind() -> Result<PathBuf, Box<dyn std::error::Error>> {
    let exe = std::env::current_exe()?;
    let dir = exe.parent().ok_or("the app executable has no folder")?;
    Ok(dir.join(format!("b3coind{}", std::env::consts::EXE_SUFFIX)))
}

pub fn run() {
    tauri::Builder::default()
        .setup(|app| {
            let config = node_config(app)?;
            let node = Node::start(&config);
            app.manage(Arc::new(Core {
                node: Arc::clone(&node),
                drafts: Mutex::new(DraftStore::default()),
            }));
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
                    let _ = window.emit_to(
                        MAIN,
                        "draft-result",
                        DraftResult::Rejected { id: draft.id },
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
            wallet_overview,
            dev_fund,
            draft_send,
            open_console,
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
                    core.node.stop(STOP_TIMEOUT);
                }
            }
        });
}
