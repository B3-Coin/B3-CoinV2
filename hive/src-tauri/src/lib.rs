//! The trusted core of the B3 Hive app. Windows only ask; the core holds the
//! node credentials and decides what reaches b3coind.

pub mod node;
pub mod rpc;

use std::path::PathBuf;
use std::sync::Arc;
use std::thread;
use std::time::Duration;

use tauri::{Emitter, Manager, RunEvent};

use node::{Node, NodeConfig, NodeStatus};

const START_TIMEOUT: Duration = Duration::from_secs(600);
const STOP_TIMEOUT: Duration = Duration::from_secs(120);

#[tauri::command]
fn node_status(node: tauri::State<'_, Arc<Node>>) -> NodeStatus {
    node.status()
}

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
            app.manage(Arc::clone(&node));
            let handle = app.handle().clone();
            thread::spawn(move || {
                node.wait_ready(START_TIMEOUT, |status| {
                    let _ = handle.emit("node-status", status);
                });
            });
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![node_status])
        .build(tauri::generate_context!())
        .expect("failed to build the B3 Hive app")
        .run(|app, event| {
            if let RunEvent::Exit = event {
                if let Some(node) = app.try_state::<Arc<Node>>() {
                    node.stop(STOP_TIMEOUT);
                }
            }
        });
}
