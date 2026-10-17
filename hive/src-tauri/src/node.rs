//! Supervises the bundled b3coind: starts it with per-launch credentials on a
//! free loopback port, follows init through the warmup error, and stops it.
//!
//! Spike scope: regtest only, with no peers (-listen=0 -connect=0 -dnsseed=0),
//! so development never touches a real B3 network or datadir.

use std::io::Read;
use std::net::TcpListener;
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use serde::Serialize;
use serde_json::{json, Value};

use crate::rpc::{Client, Credentials, RpcError, RPC_IN_WARMUP};

/// The methods the pages' login may call. Grows with b3ui/1.
/// generatetoaddress only serves the regtest funding button.
const PAGE_METHODS: &[&str] = &[
    "getblockchaininfo",
    "stop",
    "listwallets",
    "loadwallet",
    "createwallet",
    "getbalances",
    "getnewaddress",
    "walletcreatefundedpsbt",
    "walletprocesspsbt",
    "finalizepsbt",
    "decoderawtransaction",
    "sendrawtransaction",
    "generatetoaddress",
];

const POLL_INTERVAL: Duration = Duration::from_millis(250);
const STDERR_TAIL: usize = 4096;

#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(tag = "phase", rename_all = "lowercase")]
pub enum NodeStatus {
    Starting { message: String },
    Ready { chain: String, blocks: u64, best_block_hash: String },
    Stopping,
    Failed { error: String },
}

#[derive(Debug, Clone)]
pub struct NodeConfig {
    pub b3coind: PathBuf,
    pub datadir: PathBuf,
}

pub struct Node {
    child: Mutex<Option<Child>>,
    client: Option<Client>,
    status: Mutex<NodeStatus>,
    stderr_tail: Arc<Mutex<Vec<u8>>>,
    datadir: PathBuf,
    port: u16,
}

impl Node {
    /// Starts b3coind. Never fails outright: a start error becomes a Failed
    /// status the window can show.
    pub fn start(config: &NodeConfig) -> Arc<Node> {
        let stderr_tail = Arc::new(Mutex::new(Vec::new()));
        match spawn(config, &stderr_tail) {
            Ok((child, client, port)) => Arc::new(Node {
                child: Mutex::new(Some(child)),
                client: Some(client),
                status: Mutex::new(NodeStatus::Starting { message: "Starting b3coind".into() }),
                stderr_tail,
                datadir: config.datadir.clone(),
                port,
            }),
            Err(error) => Arc::new(Node {
                child: Mutex::new(None),
                client: None,
                status: Mutex::new(NodeStatus::Failed { error }),
                stderr_tail,
                datadir: config.datadir.clone(),
                port: 0,
            }),
        }
    }

    pub fn status(&self) -> NodeStatus {
        self.status.lock().unwrap().clone()
    }

    /// The pages' RPC client, once the node is ready.
    pub fn client(&self) -> Option<&Client> {
        match self.status() {
            NodeStatus::Ready { .. } => self.client.as_ref(),
            _ => None,
        }
    }

    pub fn chain(&self) -> Option<String> {
        match self.status() {
            NodeStatus::Ready { chain, .. } => Some(chain),
            _ => None,
        }
    }

    pub fn rpc_port(&self) -> u16 {
        self.port
    }

    pub fn datadir(&self) -> &std::path::Path {
        &self.datadir
    }

    /// Polls until init finishes, the process dies or `timeout` passes, and
    /// reports every status change to `on_change`.
    pub fn wait_ready(&self, timeout: Duration, mut on_change: impl FnMut(&NodeStatus)) -> NodeStatus {
        let Some(client) = &self.client else { return self.status() };
        let deadline = Instant::now() + timeout;
        loop {
            let next = match client.call("getblockchaininfo", json!([])) {
                Ok(info) => ready_status(&info),
                Err(RpcError::Rpc { code: RPC_IN_WARMUP, message }) => NodeStatus::Starting { message },
                Err(RpcError::Transport(_)) => match self.exit_status() {
                    Some(exit) => NodeStatus::Failed { error: self.exit_message(&exit) },
                    None => NodeStatus::Starting { message: "Waiting for b3coind to open RPC".into() },
                },
                Err(other) => NodeStatus::Failed { error: other.to_string() },
            };
            let next = match next {
                NodeStatus::Starting { .. } if Instant::now() >= deadline => {
                    NodeStatus::Failed { error: format!("b3coind did not finish starting within {}s", timeout.as_secs()) }
                }
                next => next,
            };
            self.set_status(next.clone(), &mut on_change);
            if !matches!(next, NodeStatus::Starting { .. }) {
                return next;
            }
            thread::sleep(POLL_INTERVAL);
        }
    }

    /// Asks b3coind to stop, waits for it to exit, and kills it after `timeout`.
    pub fn stop(&self, timeout: Duration) {
        let mut guard = self.child.lock().unwrap();
        let Some(child) = guard.as_mut() else { return };
        *self.status.lock().unwrap() = NodeStatus::Stopping;
        if let Some(client) = &self.client {
            let _ = client.call("stop", json!([]));
        }
        let deadline = Instant::now() + timeout;
        while Instant::now() < deadline {
            if let Ok(Some(_)) = child.try_wait() {
                *guard = None;
                return;
            }
            thread::sleep(POLL_INTERVAL);
        }
        let _ = child.kill();
        let _ = child.wait();
        *guard = None;
    }

    /// True once the process has exited (or never started).
    pub fn has_exited(&self) -> bool {
        self.exit_status().is_some() || self.child.lock().unwrap().is_none()
    }

    fn set_status(&self, next: NodeStatus, on_change: &mut impl FnMut(&NodeStatus)) {
        let mut current = self.status.lock().unwrap();
        if *current != next {
            *current = next.clone();
            drop(current);
            on_change(&next);
        }
    }

    fn exit_status(&self) -> Option<std::process::ExitStatus> {
        self.child.lock().unwrap().as_mut().and_then(|c| c.try_wait().ok().flatten())
    }

    fn exit_message(&self, exit: &std::process::ExitStatus) -> String {
        let tail = self.stderr_tail.lock().unwrap();
        let said = String::from_utf8_lossy(&tail);
        let said = said.trim();
        if said.is_empty() {
            format!("b3coind exited ({exit}); see debug.log in {}", self.datadir.display())
        } else {
            // e.g. the data directory lock error when another node holds it.
            format!("b3coind exited ({exit}): {said}")
        }
    }
}

fn spawn(config: &NodeConfig, stderr_tail: &Arc<Mutex<Vec<u8>>>) -> Result<(Child, Client, u16), String> {
    std::fs::create_dir_all(&config.datadir)
        .map_err(|e| format!("cannot create {}: {e}", config.datadir.display()))?;
    let credentials = Credentials::generate("hive").map_err(|e| format!("no system randomness: {e}"))?;
    let port = free_loopback_port().map_err(|e| format!("no free loopback port: {e}"))?;

    let mut child = Command::new(&config.b3coind)
        .args(node_args(config, &credentials, port))
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| format!("cannot start {}: {e}", config.b3coind.display()))?;

    // Keep the last few KB of stderr: init errors (such as the datadir lock)
    // are printed there before the process exits.
    if let Some(mut stderr) = child.stderr.take() {
        let tail = Arc::clone(stderr_tail);
        thread::spawn(move || {
            let mut buf = [0u8; 1024];
            while let Ok(n) = stderr.read(&mut buf) {
                if n == 0 {
                    break;
                }
                let mut tail = tail.lock().unwrap();
                tail.extend_from_slice(&buf[..n]);
                let excess = tail.len().saturating_sub(STDERR_TAIL);
                tail.drain(..excess);
            }
        });
    }
    Ok((child, Client::new(port, &credentials), port))
}

fn node_args(config: &NodeConfig, credentials: &Credentials, port: u16) -> Vec<String> {
    vec![
        "-regtest".into(),
        format!("-datadir={}", config.datadir.display()),
        "-server=1".into(),
        "-printtoconsole=0".into(),
        "-listen=0".into(),
        "-connect=0".into(),
        "-dnsseed=0".into(),
        "-rpcbind=127.0.0.1".into(),
        "-rpcallowip=127.0.0.1".into(),
        format!("-rpcport={port}"),
        format!("-rpcauth={}", credentials.rpcauth()),
        format!("-rpcwhitelist={}:{}", credentials.user(), PAGE_METHODS.join(",")),
        // Users without a whitelist (the cookie for b3coin-cli) keep full access.
        "-rpcwhitelistdefault=0".into(),
        // Plain regtest has no fee history; use the legacy relay floor
        // (600,000 base units per kvB) that B3 mainnet wallets default to.
        "-fallbackfee=0.0006".into(),
    ]
}

/// Asks the OS for a free port. Another process could take it before
/// b3coind binds it; the start then fails visibly and can be retried.
fn free_loopback_port() -> std::io::Result<u16> {
    Ok(TcpListener::bind("127.0.0.1:0")?.local_addr()?.port())
}

fn ready_status(info: &Value) -> NodeStatus {
    NodeStatus::Ready {
        chain: info.get("chain").and_then(Value::as_str).unwrap_or("unknown").to_owned(),
        blocks: info.get("blocks").and_then(Value::as_u64).unwrap_or(0),
        best_block_hash: info.get("bestblockhash").and_then(Value::as_str).unwrap_or("").to_owned(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn args_keep_the_node_on_regtest_without_peers() {
        let config = NodeConfig { b3coind: "b3coind".into(), datadir: "/tmp/hive".into() };
        let credentials = Credentials::generate("hive").unwrap();
        let args = node_args(&config, &credentials, 18443);
        for required in ["-regtest", "-listen=0", "-connect=0", "-dnsseed=0", "-rpcbind=127.0.0.1", "-rpcwhitelistdefault=0"] {
            assert!(args.iter().any(|a| a == required), "missing {required}");
        }
        assert!(args.contains(&format!("-rpcauth={}", credentials.rpcauth())));
        let whitelist = args.iter().find(|a| a.starts_with("-rpcwhitelist=hive:")).unwrap();
        for method in ["getblockchaininfo", "stop", "sendrawtransaction"] {
            assert!(whitelist.split([':', ',']).any(|m| m == method), "{method} not whitelisted");
        }
        for never in ["listdescriptors", "gethdkeys", "exportflowmeshkey", "dumpprivkey"] {
            assert!(!whitelist.contains(never), "{never} must not be whitelisted");
        }
    }

    #[test]
    fn status_serializes_with_a_phase_tag() {
        let ready = NodeStatus::Ready { chain: "regtest".into(), blocks: 0, best_block_hash: "ab".into() };
        assert_eq!(
            serde_json::to_value(ready).unwrap(),
            json!({"phase": "ready", "chain": "regtest", "blocks": 0, "best_block_hash": "ab"})
        );
        assert_eq!(serde_json::to_value(NodeStatus::Stopping).unwrap(), json!({"phase": "stopping"}));
    }

    #[test]
    fn a_missing_binary_becomes_a_failed_status() {
        let dir = std::env::temp_dir().join(format!("hive-missing-{}", std::process::id()));
        let node = Node::start(&NodeConfig { b3coind: dir.join("no-such-b3coind"), datadir: dir.clone() });
        assert!(matches!(node.status(), NodeStatus::Failed { .. }));
        assert!(node.has_exited());
        let _ = std::fs::remove_dir_all(dir);
    }
}
