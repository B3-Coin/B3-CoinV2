//! Starts a real b3coind on regtest with no peers. Runs only when
//! B3_HIVE_B3COIND names the binary:
//!   B3_HIVE_B3COIND=/path/to/b3coind cargo test --test node_regtest -- --test-threads=1

use std::path::PathBuf;
use std::sync::{mpsc, Arc};
use std::thread;
use std::time::Duration;

use serde_json::json;

use b3_hive::chain;
use b3_hive::console;
use b3_hive::drafts::DraftStore;
use b3_hive::node::{Node, NodeConfig, NodeStatus};
use b3_hive::rpc::{Client, RpcError};
use b3_hive::wallet;

/// A ready regtest node in a fresh temp datadir, stopped and removed on drop.
struct TestNode {
    node: Arc<Node>,
    datadir: PathBuf,
}

impl TestNode {
    fn start(name: &str) -> Option<(Self, NodeStatus)> {
        let Some(b3coind) = std::env::var_os("B3_HIVE_B3COIND") else {
            eprintln!("skipped: B3_HIVE_B3COIND is not set");
            return None;
        };
        let datadir = std::env::temp_dir().join(format!("hive-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&datadir);
        let node = Node::start(&NodeConfig {
            b3coind: b3coind.into(),
            datadir: datadir.clone(),
        });
        let status = node.wait_ready(Duration::from_secs(300), |_| {});
        Some((TestNode { node, datadir }, status))
    }

    /// A full-access client via b3coind's cookie, for checking what the page cannot see.
    fn admin(&self) -> Client {
        let cookie = std::fs::read_to_string(self.datadir.join("regtest").join(".cookie"))
            .expect("regtest cookie");
        Client::from_cookie(self.node.rpc_port(), &cookie)
    }

    fn mempool(&self) -> Vec<String> {
        let m = self.admin().call("getrawmempool", json!([])).unwrap();
        m.as_array()
            .unwrap()
            .iter()
            .map(|t| t.as_str().unwrap().to_owned())
            .collect()
    }
}

impl Drop for TestNode {
    fn drop(&mut self) {
        self.node.stop(Duration::from_secs(120));
        let _ = std::fs::remove_dir_all(&self.datadir);
    }
}

#[test]
fn starts_reports_ready_and_stops() {
    let Some((test, status)) = TestNode::start("start") else {
        return;
    };
    match &status {
        NodeStatus::Ready {
            chain,
            blocks,
            best_block_hash,
        } => {
            assert_eq!(chain, "regtest");
            assert_eq!(*blocks, 0);
            assert_eq!(best_block_hash.len(), 64);
        }
        other => panic!("node did not become ready: {other:?}"),
    }
    test.node.stop(Duration::from_secs(120));
    assert!(test.node.has_exited());
}

#[test]
fn the_page_login_reads_the_dashboard_network_and_activity() {
    let Some((test, status)) = TestNode::start("pages") else {
        return;
    };
    assert!(
        matches!(status, NodeStatus::Ready { .. }),
        "not ready: {status:?}"
    );
    let client = test.node.client().unwrap();

    let o = chain::overview(client).unwrap();
    assert_eq!(o.chain, "regtest");
    assert_eq!((o.blocks, o.headers), (0, 0));
    assert_eq!(o.best_block_hash.len(), 64);
    assert!(o.user_agent.starts_with('/'), "{}", o.user_agent);
    // No peers, and nothing waiting.
    assert_eq!((o.connections_in, o.connections_out), (0, 0));
    assert_eq!(o.mempool_transactions, 0);
    assert!(chain::peers(client).unwrap().is_empty());

    wallet::ensure_wallet(client).unwrap();
    assert!(wallet::activity(client, 10).unwrap().is_empty());
    wallet::fund_regtest(client).unwrap();
    let lines = wallet::activity(client, 10).unwrap();
    assert_eq!(lines.len(), 10);
    // Newest first: the last block mined has one confirmation.
    assert_eq!(lines[0].confirmations, 1);
    assert_eq!(lines[0].category, "immature");
    assert!(lines
        .windows(2)
        .all(|w| w[0].confirmations <= w[1].confirmations));
    assert_eq!(chain::overview(client).unwrap().blocks, wallet::FUND_BLOCKS);
}

#[test]
fn follow_reports_new_blocks_and_an_exit() {
    let Some((test, status)) = TestNode::start("follow") else {
        return;
    };
    assert!(
        matches!(status, NodeStatus::Ready { .. }),
        "not ready: {status:?}"
    );
    let (tx, rx) = mpsc::channel();
    let node = Arc::clone(&test.node);
    let follower = thread::spawn(move || {
        node.follow(Duration::from_millis(100), |s| {
            let _ = tx.send(s.clone());
        })
    });
    let next = || {
        rx.recv_timeout(Duration::from_secs(30))
            .expect("a status change")
    };

    let admin = test.admin();
    admin.call("createwallet", json!(["follow"])).unwrap();
    let to = admin
        .call_wallet("follow", "getnewaddress", json!([]))
        .unwrap();
    admin.call("generatetoaddress", json!([1, to])).unwrap();
    match next() {
        NodeStatus::Ready { blocks, .. } => assert_eq!(blocks, 1),
        other => panic!("expected the new tip, got {other:?}"),
    }

    // A stop that the app did not ask for, as from the console.
    admin.call("stop", json!([])).unwrap();
    match next() {
        NodeStatus::Failed { error } => assert!(error.contains("stopped"), "{error}"),
        other => panic!("expected the exit, got {other:?}"),
    }
    follower.join().unwrap();
}

#[test]
fn a_drafted_send_is_submitted_exactly_once() {
    let Some((test, status)) = TestNode::start("send") else {
        return;
    };
    assert!(
        matches!(status, NodeStatus::Ready { .. }),
        "not ready: {status:?}"
    );
    let client = test.node.client().expect("ready node has a client");

    wallet::ensure_wallet(client).unwrap();
    wallet::ensure_wallet(client).unwrap(); // loading twice is fine
    wallet::fund_regtest(client).unwrap();
    let (spendable, _) = wallet::balances(client).unwrap();
    assert!(
        !spendable.trim_start_matches(['0', '.']).is_empty(),
        "nothing spendable: {spendable}"
    );

    let to = client
        .call_wallet(wallet::SPIKE_WALLET, "getnewaddress", json!([]))
        .unwrap();
    let to = to.as_str().unwrap().to_owned();
    let (summary, hex) = wallet::prepare_send(client, "regtest", &to, "0.123456789").unwrap();

    // The summary is the core's decoding of the exact bytes it will submit.
    let paid: Vec<_> = summary.outputs.iter().filter(|o| !o.change).collect();
    assert_eq!(paid.len(), 1);
    assert_eq!(paid[0].address, to);
    assert_eq!(paid[0].amount, "0.123456789");
    assert!(summary.outputs.iter().any(|o| o.change));
    assert_eq!(summary.txid.len(), 64);

    // Nothing is broadcast while the draft waits.
    assert!(test.mempool().is_empty());

    let mut store = DraftStore::default();
    let id = store.insert(summary.clone(), hex).unwrap();
    let draft = store.take(&id).unwrap();
    assert_eq!(wallet::broadcast(client, &draft.hex).unwrap(), summary.txid);
    assert!(store.take(&id).is_none(), "a decided draft must be gone");
    assert_eq!(test.mempool(), vec![summary.txid.clone()]);

    assert!(wallet::prepare_send(client, "regtest", &to, "1.0000000001").is_err());
    assert!(wallet::prepare_send(client, "regtest", "not-an-address", "1").is_err());
}

#[test]
fn the_page_login_cannot_export_keys() {
    let Some((test, status)) = TestNode::start("whitelist") else {
        return;
    };
    assert!(
        matches!(status, NodeStatus::Ready { .. }),
        "not ready: {status:?}"
    );
    let client = test.node.client().unwrap();
    wallet::ensure_wallet(client).unwrap();
    for method in [
        "listdescriptors",
        "gethdkeys",
        "dumpprivkey",
        "signmessage",
        "stop_never_listed",
    ] {
        match client.call_wallet(wallet::SPIKE_WALLET, method, json!([])) {
            Err(RpcError::Http(403)) => {}
            other => panic!("{method} was not refused with 403: {other:?}"),
        }
    }
}

#[test]
fn the_console_runs_typed_commands_with_full_access() {
    let Some((test, status)) = TestNode::start("console") else {
        return;
    };
    assert!(
        matches!(status, NodeStatus::Ready { .. }),
        "not ready: {status:?}"
    );
    let client = test.node.console_client().unwrap();
    let mut exec = |method: &str, params: serde_json::Value| {
        client.call(method, params).map_err(|e| match e {
            RpcError::Rpc { code, message } => console::ConsoleError::Rpc { code, message },
            other => console::ConsoleError::Error(other.to_string()),
        })
    };

    let reply = console::submit("getblockchaininfo()[chain]", &mut exec).unwrap();
    assert_eq!((reply.ok, reply.output.as_str()), (true, "regtest"));
    let reply = console::submit("getblockhash(0)", &mut exec).unwrap();
    assert_eq!(reply.output.len(), 64);

    // Typed arguments convert as in b3coin-cli: the name stays a string,
    // disable_private_keys becomes a boolean.
    let reply = console::submit("createwallet 123 true", &mut exec).unwrap();
    assert!(reply.ok, "{}", reply.output);
    assert_eq!(reply.command, "createwallet(…)");
    let wallet = client
        .call_wallet("123", "getwalletinfo", json!([]))
        .unwrap();
    assert_eq!(wallet["private_keys_enabled"], json!(false));

    // The page login is refused this call (see above); the console is not.
    let reply = console::submit("listdescriptors", &mut |m: &str, p| {
        client
            .call_wallet("123", m, p)
            .map_err(|e| console::ConsoleError::Error(e.to_string()))
    })
    .unwrap();
    assert!(reply.ok, "{}", reply.output);

    let reply = console::submit("getblockhash(-5)", &mut exec).unwrap();
    assert!(!reply.ok);
    assert!(reply.output.ends_with(")"), "{}", reply.output);
}
