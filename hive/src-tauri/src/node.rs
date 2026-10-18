//! Supervises the bundled b3coind: starts it with per-launch credentials on a
//! free loopback port, follows init through the warmup error, follows the
//! running node's tip until it exits, and stops it.
//!
//! B3 mainnet by default. Regtest is a developer option and runs with no
//! peers (-listen=0 -connect=0 -dnsseed=0), so development never touches a
//! real B3 network or datadir.

use std::io::Read;
use std::net::TcpListener;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use serde::Serialize;
use serde_json::{json, Value};

use crate::rpc::{Client, Credentials, RpcError, RPC_IN_WARMUP};

/// The pages' login: limited to PAGE_METHODS by -rpcwhitelist.
const PAGE_USER: &str = "hive";
/// The console's login: no whitelist, so every RPC, as in the Qt console.
/// Only the console window can reach it.
const CONSOLE_USER: &str = "hive-console";

/// The methods the pages' login may call. Grows with b3ui/1.
/// generatetoaddress only serves the regtest funding button.
const PAGE_METHODS: &[&str] = &[
    "getblockchaininfo",
    "getnetworkinfo",
    "getmempoolinfo",
    "getpeerinfo",
    "getnettotals",
    "listbanned",
    "uptime",
    "stop",
    "listwallets",
    "listwalletdir",
    "loadwallet",
    "createwallet",
    "unloadwallet",
    "backupwallet",
    "encryptwallet",
    "walletpassphrasechange",
    "walletlock",
    "getbalances",
    "getwalletinfo",
    "getstakinginfo",
    "listtransactions",
    "gettransaction",
    "getnewaddress",
    "listreceivedbyaddress",
    "walletcreatefundedpsbt",
    "walletprocesspsbt",
    "analyzepsbt",
    "finalizepsbt",
    "decodepsbt",
    "lockunspent",
    "walletpassphrase",
    "listunspent",
    "setlabel",
    "listlabels",
    "getaddressesbylabel",
    "getaddressinfo",
    "getstakinginfo",
    "getfinalityinfo",
    "createstake",
    "bindfinalitykey",
    "startstaking",
    "stopstaking",
    "decoderawtransaction",
    "sendrawtransaction",
    "generatetoaddress",
];

const POLL_INTERVAL: Duration = Duration::from_millis(250);
const STDERR_TAIL: usize = 4096;

#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(tag = "phase", rename_all = "lowercase")]
pub enum NodeStatus {
    Starting {
        message: String,
    },
    Ready {
        chain: String,
        blocks: u64,
        best_block_hash: String,
    },
    Stopping,
    Failed {
        error: String,
    },
    /// Not started: a fresh mainnet data folder waits for the setup
    /// wizard's choice between a bootstrap and syncing from peers.
    Setup,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Network {
    Main,
    Regtest,
}

impl Network {
    /// B3_HIVE_NETWORK: unset or "main" for mainnet, "regtest" for development.
    pub fn from_setting(value: Option<&str>) -> Result<Self, String> {
        match value.map(str::trim) {
            None | Some("") | Some("main") => Ok(Network::Main),
            Some("regtest") => Ok(Network::Regtest),
            Some(other) => Err(format!(
                "B3_HIVE_NETWORK must be main or regtest, not {other:?}"
            )),
        }
    }

    /// Where the chain data lives inside the data folder: mainnet uses the
    /// folder itself, as the legacy client does.
    pub fn chain_dir(self, datadir: &Path) -> PathBuf {
        match self {
            Network::Main => datadir.to_path_buf(),
            Network::Regtest => datadir.join("regtest"),
        }
    }
}

#[derive(Debug, Clone)]
pub struct NodeConfig {
    pub b3coind: PathBuf,
    pub datadir: PathBuf,
    pub network: Network,
}

pub struct Node {
    child: Mutex<Option<Child>>,
    client: Option<Client>,
    console: Option<Client>,
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
            Ok(Spawned {
                child,
                client,
                console,
                port,
            }) => Arc::new(Node {
                child: Mutex::new(Some(child)),
                client: Some(client),
                console: Some(console),
                status: Mutex::new(NodeStatus::Starting {
                    message: "Starting b3coind".into(),
                }),
                stderr_tail,
                datadir: config.datadir.clone(),
                port,
            }),
            Err(error) => Arc::new(Node {
                child: Mutex::new(None),
                client: None,
                console: None,
                status: Mutex::new(NodeStatus::Failed { error }),
                stderr_tail,
                datadir: config.datadir.clone(),
                port: 0,
            }),
        }
    }

    /// A node that waits for the setup wizard; nothing runs yet.
    pub fn waiting_for_setup(config: &NodeConfig) -> Arc<Node> {
        Arc::new(Node {
            child: Mutex::new(None),
            client: None,
            console: None,
            status: Mutex::new(NodeStatus::Setup),
            stderr_tail: Arc::new(Mutex::new(Vec::new())),
            datadir: config.datadir.clone(),
            port: 0,
        })
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

    /// The console's full-access client. Unlike the pages' client it is
    /// available while the node starts, so warmup errors show in the console.
    pub fn console_client(&self) -> Option<&Client> {
        self.console.as_ref()
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
    pub fn wait_ready(
        &self,
        timeout: Duration,
        mut on_change: impl FnMut(&NodeStatus),
    ) -> NodeStatus {
        let Some(client) = &self.client else {
            return self.status();
        };
        let deadline = Instant::now() + timeout;
        loop {
            let next = match client.call("getblockchaininfo", json!([])) {
                Ok(info) => ready_status(&info),
                Err(RpcError::Rpc {
                    code: RPC_IN_WARMUP,
                    message,
                }) => NodeStatus::Starting { message },
                Err(RpcError::Transport(_)) => match self.exit_status() {
                    Some(exit) => NodeStatus::Failed {
                        error: self.exit_message(&exit),
                    },
                    None => NodeStatus::Starting {
                        message: "Waiting for b3coind to open RPC".into(),
                    },
                },
                Err(other) => NodeStatus::Failed {
                    error: other.to_string(),
                },
            };
            let next = match next {
                NodeStatus::Starting { .. } if Instant::now() >= deadline => NodeStatus::Failed {
                    error: format!(
                        "b3coind did not finish starting within {}s",
                        timeout.as_secs()
                    ),
                },
                next => next,
            };
            self.set_status(next.clone(), &mut on_change);
            if !matches!(next, NodeStatus::Starting { .. }) {
                return next;
            }
            thread::sleep(POLL_INTERVAL);
        }
    }

    /// Follows a ready node until it exits or `stop` is called: every
    /// `interval` it reads the tip, and reports new blocks and an exit to
    /// `on_change`. A slow reply from a live process keeps the last status.
    pub fn follow(&self, interval: Duration, mut on_change: impl FnMut(&NodeStatus)) {
        let Some(client) = &self.client else { return };
        loop {
            thread::sleep(interval);
            if matches!(self.status(), NodeStatus::Stopping) {
                return;
            }
            let next = match client.call("getblockchaininfo", json!([])) {
                Ok(info) => ready_status(&info),
                Err(_) => match self.exit_status() {
                    Some(exit) => NodeStatus::Failed {
                        error: self.exit_message(&exit),
                    },
                    None => continue,
                },
            };
            self.set_status(next, &mut on_change);
            if !matches!(self.status(), NodeStatus::Ready { .. }) {
                return;
            }
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

    /// Stopping is final: a poll that raced with `stop` must not undo it.
    fn set_status(&self, next: NodeStatus, on_change: &mut impl FnMut(&NodeStatus)) {
        let mut current = self.status.lock().unwrap();
        if *current != next && !matches!(*current, NodeStatus::Stopping) {
            *current = next.clone();
            drop(current);
            on_change(&next);
        }
    }

    fn exit_status(&self) -> Option<std::process::ExitStatus> {
        self.child
            .lock()
            .unwrap()
            .as_mut()
            .and_then(|c| c.try_wait().ok().flatten())
    }

    fn exit_message(&self, exit: &std::process::ExitStatus) -> String {
        let tail = self.stderr_tail.lock().unwrap();
        let said = String::from_utf8_lossy(&tail);
        let said = said.trim();
        if said.is_empty() && exit.success() {
            // For example after `stop` in the console.
            "b3coind stopped. Restart B3 Hive to start it again.".into()
        } else if said.is_empty() {
            format!(
                "b3coind exited ({exit}); see debug.log in {}",
                self.datadir.display()
            )
        } else {
            // e.g. the data directory lock error when another node holds it.
            format!("b3coind exited ({exit}): {said}")
        }
    }
}

/// b3coind and the update helper are console programs. Started from the
/// windowless release app, Windows would open a console window for each.
#[cfg(windows)]
pub(crate) fn hide_console(command: &mut Command) {
    use std::os::windows::process::CommandExt;
    const CREATE_NO_WINDOW: u32 = 0x0800_0000;
    command.creation_flags(CREATE_NO_WINDOW);
}

#[cfg(not(windows))]
pub(crate) fn hide_console(_command: &mut Command) {}

struct Spawned {
    child: Child,
    client: Client,
    console: Client,
    port: u16,
}

fn spawn(config: &NodeConfig, stderr_tail: &Arc<Mutex<Vec<u8>>>) -> Result<Spawned, String> {
    std::fs::create_dir_all(&config.datadir)
        .map_err(|e| format!("cannot create {}: {e}", config.datadir.display()))?;
    let no_randomness = |e| format!("no system randomness: {e}");
    let page = Credentials::generate(PAGE_USER).map_err(no_randomness)?;
    let console = Credentials::generate(CONSOLE_USER).map_err(no_randomness)?;
    let port = free_loopback_port().map_err(|e| format!("no free loopback port: {e}"))?;

    let mut command = Command::new(&config.b3coind);
    command
        .args(node_args(config, &page, &console, port))
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::piped());
    hide_console(&mut command);
    let mut child = command
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
    Ok(Spawned {
        child,
        client: Client::new(port, &page),
        console: Client::without_timeout(port, &console),
        port,
    })
}

fn node_args(
    config: &NodeConfig,
    page: &Credentials,
    console: &Credentials,
    port: u16,
) -> Vec<String> {
    let mut args: Vec<String> = match config.network {
        Network::Main => vec![],
        Network::Regtest => vec![
            "-regtest".into(),
            "-listen=0".into(),
            "-connect=0".into(),
            "-dnsseed=0".into(),
        ],
    };
    args.extend([
        format!("-datadir={}", config.datadir.display()),
        "-server=1".into(),
        "-printtoconsole=0".into(),
        "-rpcbind=127.0.0.1".into(),
        "-rpcallowip=127.0.0.1".into(),
        format!("-rpcport={port}"),
        format!("-rpcauth={}", page.rpcauth()),
        format!("-rpcwhitelist={}:{}", page.user(), PAGE_METHODS.join(",")),
        // The console login has no whitelist entry, so it keeps full access,
        // as does the cookie for b3coin-cli.
        format!("-rpcauth={}", console.rpcauth()),
        "-rpcwhitelistdefault=0".into(),
        // Without fee history (regtest, a quiet mainnet) use the legacy relay
        // floor (600,000 base units per kvB) that B3 mainnet wallets default to.
        "-fallbackfee=0.0006".into(),
    ]);
    args
}

/// Asks the OS for a free port. Another process could take it before
/// b3coind binds it; the start then fails visibly and can be retried.
fn free_loopback_port() -> std::io::Result<u16> {
    Ok(TcpListener::bind("127.0.0.1:0")?.local_addr()?.port())
}

fn ready_status(info: &Value) -> NodeStatus {
    NodeStatus::Ready {
        chain: info
            .get("chain")
            .and_then(Value::as_str)
            .unwrap_or("unknown")
            .to_owned(),
        blocks: info.get("blocks").and_then(Value::as_u64).unwrap_or(0),
        best_block_hash: info
            .get("bestblockhash")
            .and_then(Value::as_str)
            .unwrap_or("")
            .to_owned(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn args_keep_the_node_on_regtest_without_peers() {
        let config = NodeConfig {
            b3coind: "b3coind".into(),
            datadir: "/tmp/hive".into(),
            network: Network::Regtest,
        };
        let credentials = Credentials::generate(PAGE_USER).unwrap();
        let console = Credentials::generate(CONSOLE_USER).unwrap();
        let args = node_args(&config, &credentials, &console, 18443);
        for required in [
            "-regtest",
            "-listen=0",
            "-connect=0",
            "-dnsseed=0",
            "-rpcbind=127.0.0.1",
            "-rpcwhitelistdefault=0",
        ] {
            assert!(args.iter().any(|a| a == required), "missing {required}");
        }
        assert!(args.contains(&format!("-rpcauth={}", credentials.rpcauth())));
        assert!(args.contains(&format!("-rpcauth={}", console.rpcauth())));
        // Only the page login is whitelisted; the console keeps full access.
        let whitelists: Vec<_> = args
            .iter()
            .filter(|a| a.starts_with("-rpcwhitelist="))
            .collect();
        assert_eq!(whitelists.len(), 1);
        let whitelist = args
            .iter()
            .find(|a| a.starts_with("-rpcwhitelist=hive:"))
            .unwrap();
        for method in ["getblockchaininfo", "stop", "sendrawtransaction"] {
            assert!(
                whitelist.split([':', ',']).any(|m| m == method),
                "{method} not whitelisted"
            );
        }
        for never in [
            "listdescriptors",
            "gethdkeys",
            "exportflowmeshkey",
            "dumpprivkey",
        ] {
            assert!(
                !whitelist.contains(never),
                "{never} must not be whitelisted"
            );
        }
    }

    #[test]
    fn mainnet_args_have_no_chain_flag_and_keep_peers() {
        let config = NodeConfig {
            b3coind: "b3coind".into(),
            datadir: "/home/u/.B3-CoinV2".into(),
            network: Network::Main,
        };
        let page = Credentials::generate(PAGE_USER).unwrap();
        let console = Credentials::generate(CONSOLE_USER).unwrap();
        let args = node_args(&config, &page, &console, 5000);
        for absent in [
            "-regtest",
            "-listen=0",
            "-connect=0",
            "-dnsseed=0",
            "-testnet",
        ] {
            assert!(!args.iter().any(|a| a == absent), "{absent} on mainnet");
        }
        assert!(args.contains(&"-datadir=/home/u/.B3-CoinV2".to_owned()));
        // RPC stays on loopback with the per-launch logins.
        assert!(args.contains(&"-rpcbind=127.0.0.1".to_owned()));
        assert!(args.contains(&"-rpcwhitelistdefault=0".to_owned()));
    }

    #[test]
    fn the_network_setting_defaults_to_mainnet() {
        assert_eq!(Network::from_setting(None), Ok(Network::Main));
        assert_eq!(Network::from_setting(Some("main")), Ok(Network::Main));
        assert_eq!(Network::from_setting(Some("regtest")), Ok(Network::Regtest));
        assert!(Network::from_setting(Some("testnet")).is_err());
        let dir = Path::new("/d");
        assert_eq!(Network::Main.chain_dir(dir), PathBuf::from("/d"));
        assert_eq!(Network::Regtest.chain_dir(dir), PathBuf::from("/d/regtest"));
    }

    #[test]
    fn status_serializes_with_a_phase_tag() {
        let ready = NodeStatus::Ready {
            chain: "regtest".into(),
            blocks: 0,
            best_block_hash: "ab".into(),
        };
        assert_eq!(
            serde_json::to_value(ready).unwrap(),
            json!({"phase": "ready", "chain": "regtest", "blocks": 0, "best_block_hash": "ab"})
        );
        assert_eq!(
            serde_json::to_value(NodeStatus::Stopping).unwrap(),
            json!({"phase": "stopping"})
        );
    }

    #[test]
    fn a_missing_binary_becomes_a_failed_status() {
        let dir = std::env::temp_dir().join(format!("hive-missing-{}", std::process::id()));
        let node = Node::start(&NodeConfig {
            b3coind: dir.join("no-such-b3coind"),
            datadir: dir.clone(),
            network: Network::Regtest,
        });
        assert!(matches!(node.status(), NodeStatus::Failed { .. }));
        assert!(node.has_exited());
        let _ = std::fs::remove_dir_all(dir);
    }
}
