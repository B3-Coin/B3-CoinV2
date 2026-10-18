//! Starts a real b3coind on regtest with no peers. Runs only when
//! B3_HIVE_B3COIND names the binary:
//!   B3_HIVE_B3COIND=/path/to/b3coind cargo test --test node_regtest -- --test-threads=1

use std::path::{Path, PathBuf};
use std::sync::atomic::AtomicBool;
use std::sync::{mpsc, Arc};
use std::thread;
use std::time::Duration;

use serde_json::json;

use b3_hive::bootstrap;
use b3_hive::chain;
use b3_hive::console;
use b3_hive::drafts::DraftStore;
use b3_hive::history::{self, Filter, Kind};
use b3_hive::node::{Network, Node, NodeConfig, NodeStatus};
use b3_hive::receive;
use b3_hive::rpc::{Client, RpcError};
use b3_hive::settings::{self, NodeSettings};
use b3_hive::wallet;

const W: &str = wallet::SPIKE_WALLET;

/// A ready regtest node in a fresh temp datadir, stopped and removed on drop.
struct TestNode {
    node: Arc<Node>,
    datadir: PathBuf,
}

impl TestNode {
    fn start(name: &str) -> Option<(Self, NodeStatus)> {
        Self::start_prepared(name, |_| {})
    }

    /// Like start, after `prepare` has filled the empty datadir.
    fn start_prepared(name: &str, prepare: impl FnOnce(&Path)) -> Option<(Self, NodeStatus)> {
        let Some(b3coind) = std::env::var_os("B3_HIVE_B3COIND") else {
            eprintln!("skipped: B3_HIVE_B3COIND is not set");
            return None;
        };
        let datadir = std::env::temp_dir().join(format!("hive-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&datadir);
        std::fs::create_dir_all(&datadir).unwrap();
        prepare(&datadir);
        let node = Node::start(&NodeConfig {
            b3coind: b3coind.into(),
            datadir: datadir.clone(),
            network: Network::Regtest,
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

    // The Network page's Information rows, and a ban made in the console.
    let d = chain::details(client).unwrap();
    assert!(d.banned.is_empty());
    assert!(d.mempool_max > 0);
    test.admin()
        .call("setban", json!(["192.0.2.1", "add", 3600]))
        .unwrap();
    let d = chain::details(client).unwrap();
    assert_eq!(d.banned.len(), 1);
    assert_eq!(d.banned[0].address, "192.0.2.1/32");
    assert!(d.banned[0].until > d.banned[0].created);

    // Pages load the wallet at the same time; every one of them succeeds.
    thread::scope(|s| {
        let loads: Vec<_> = (0..4)
            .map(|_| s.spawn(|| wallet::ensure_wallet(client)))
            .collect();
        for load in loads {
            load.join().unwrap().unwrap();
        }
    });
    assert_eq!(
        wallet::select(client, "regtest", None).unwrap().as_deref(),
        Some(W)
    );
    assert!(wallet::activity(client, W, 10).unwrap().is_empty());
    assert_eq!(
        wallet::security(client, W).unwrap(),
        wallet::Security::Unencrypted
    );
    // Regtest has no legacy B3 boundary, so its phase is plain "modern".
    let staking = wallet::staking(client, W).unwrap();
    assert_eq!(staking.next_block_phase, "modern", "{staking:?}");
    assert!(!staking.this_wallet);
    wallet::fund_regtest(client, W).unwrap();
    let b = wallet::balances(client, W).unwrap();
    assert_eq!(b.pending.trim_start_matches(['0', '.']), "", "{b:?}");
    assert_ne!(b.immature.trim_start_matches(['0', '.']), "", "{b:?}");
    let lines = wallet::activity(client, W, 10).unwrap();
    assert_eq!(lines.len(), 10);

    // The Activity page sees the whole history, not just the newest lines.
    let all = history::all(client, W).unwrap();
    assert_eq!(all.len() as u64, wallet::FUND_BLOCKS);
    let mined = Filter {
        kind: Kind::Mined,
        ..Default::default()
    };
    let page = history::filter(all.clone(), &mined, 10).unwrap();
    assert_eq!(
        (page.entries.len(), page.matching, page.total),
        (10, 130, 130)
    );
    let sent = Filter {
        kind: Kind::Sent,
        ..Default::default()
    };
    assert_eq!(history::filter(all.clone(), &sent, 10).unwrap().matching, 0);
    let d = history::details(client, W, &all[0].txid).unwrap();
    assert_eq!(d.txid, all[0].txid);
    assert!(d.generated, "{d:?}");
    assert_eq!(d.confirmations, 1);
    assert!(d.size > 0);
    assert!(history::details(client, W, "not-a-txid").is_err());
    assert!(history::csv(&all).lines().count() == all.len() + 1);
    // Newest first: the last block mined has one confirmation.
    assert_eq!(lines[0].confirmations, 1);
    assert_eq!(lines[0].category, "immature");
    assert!(lines
        .windows(2)
        .all(|w| w[0].confirmations <= w[1].confirmations));
    assert_eq!(chain::overview(client).unwrap().blocks, wallet::FUND_BLOCKS);
}

/// A bootstrap made from one node's chain folders starts another node at
/// the same tip.
#[test]
fn a_bootstrap_installs_a_working_chain() {
    let Some((source, status)) = TestNode::start("boot-src") else {
        return;
    };
    assert!(
        matches!(status, NodeStatus::Ready { .. }),
        "not ready: {status:?}"
    );
    let client = source.node.client().unwrap();
    wallet::ensure_wallet(client).unwrap();
    wallet::fund_regtest(client, W).unwrap();
    let tip = chain::overview(client).unwrap().best_block_hash;
    source.node.stop(Duration::from_secs(120));

    // Packed as the explorer packs mainnet: the chain folders at the top.
    let chain_dir = source.datadir.join("regtest");
    let mut tar = tar::Builder::new(Vec::new());
    for name in ["blocks", "chainstate"] {
        tar.append_dir_all(name, chain_dir.join(name)).unwrap();
    }
    let bytes = zstd::encode_all(&tar.into_inner().unwrap()[..], 3).unwrap();
    let offer = bootstrap::parse_manifest(
        json!({"bootstraps": [{
            "height": wallet::FUND_BLOCKS,
            "size": bytes.len(),
            "sha256": sha256_hex(&bytes),
            "url": format!("/bootstraps/bootstrap-{}.tar.zst", wallet::FUND_BLOCKS),
        }]})
        .to_string()
        .as_bytes(),
    )
    .unwrap()
    .remove(0);

    let mut phases = Vec::new();
    let Some((target, status)) = TestNode::start_prepared("boot-dst", |datadir| {
        let chain_dir = datadir.join("regtest");
        assert!(bootstrap::is_fresh(&chain_dir));
        bootstrap::install(
            &mut &bytes[..],
            &offer,
            &chain_dir,
            &mut |p| phases.push(p),
            &AtomicBool::new(false),
        )
        .unwrap();
        assert!(!bootstrap::is_fresh(&chain_dir));
    }) else {
        return;
    };
    match status {
        NodeStatus::Ready {
            blocks,
            best_block_hash,
            ..
        } => {
            assert_eq!(blocks, wallet::FUND_BLOCKS);
            assert_eq!(best_block_hash, tip);
        }
        other => panic!("the bootstrapped node did not start: {other:?}"),
    }
    assert_eq!(
        phases.last(),
        Some(&bootstrap::Progress::Done {
            height: wallet::FUND_BLOCKS
        })
    );
    drop(target);
}

/// The proxies b3coind reports, by network name.
fn proxies(client: &Client) -> Vec<(String, String)> {
    let info = client.call("getnetworkinfo", json!([])).unwrap();
    info["networks"]
        .as_array()
        .unwrap()
        .iter()
        .map(|n| {
            (
                n["name"].as_str().unwrap().to_owned(),
                n["proxy"].as_str().unwrap_or_default().to_owned(),
            )
        })
        .collect()
}

fn proxy_of(client: &Client, network: &str) -> String {
    proxies(client)
        .into_iter()
        .find(|(name, _)| name == network)
        .map(|(_, proxy)| proxy)
        .unwrap_or_default()
}

/// b3coind reads the settings B3 Hive writes, and a restart picks up a change.
#[test]
fn node_settings_take_effect_after_a_restart() {
    let first = NodeSettings {
        proxy: Some("127.0.0.1:9050".into()),
        dbcache_mib: Some(64),
        ..Default::default()
    };
    let Some((test, status)) = TestNode::start_prepared("settings", |datadir| {
        settings::write(&datadir.join("regtest"), &first).unwrap();
    }) else {
        return;
    };
    assert!(
        matches!(status, NodeStatus::Ready { .. }),
        "not ready: {status:?}"
    );
    let client = test.node.client().unwrap();
    assert_eq!(proxy_of(client, "ipv4"), "127.0.0.1:9050");
    assert_eq!(proxy_of(client, "onion"), "127.0.0.1:9050");

    // As the settings page does: stop, write, start again.
    test.node.stop(Duration::from_secs(120));
    let chain_dir = test.datadir.join("regtest");
    let second = NodeSettings {
        onion: Some("127.0.0.1:9150".into()),
        ..Default::default()
    };
    settings::write(&chain_dir, &second).unwrap();
    assert_eq!(settings::read(&chain_dir).unwrap(), second);
    let again = Node::start(&NodeConfig {
        b3coind: std::env::var_os("B3_HIVE_B3COIND").unwrap().into(),
        datadir: test.datadir.clone(),
        network: Network::Regtest,
    });
    let status = again.wait_ready(Duration::from_secs(300), |_| {});
    assert!(
        matches!(status, NodeStatus::Ready { .. }),
        "not ready after the change: {status:?}"
    );
    let client = again.client().unwrap();
    assert_eq!(proxy_of(client, "ipv4"), "");
    assert_eq!(proxy_of(client, "onion"), "127.0.0.1:9150");
    again.stop(Duration::from_secs(120));
}

fn sha256_hex(bytes: &[u8]) -> String {
    use sha2::Digest;
    sha2::Sha256::digest(bytes)
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect()
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
    wallet::fund_regtest(client, W).unwrap();
    let spendable = wallet::balances(client, W).unwrap().spendable;
    assert!(
        !spendable.trim_start_matches(['0', '.']).is_empty(),
        "nothing spendable: {spendable}"
    );

    let to = client.call_wallet(W, "getnewaddress", json!([])).unwrap();
    let to = to.as_str().unwrap().to_owned();
    let (summary, hex) = wallet::prepare_send(client, W, "regtest", &to, "0.123456789").unwrap();

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

    assert!(wallet::prepare_send(client, W, "regtest", &to, "1.0000000001").is_err());
    assert!(wallet::prepare_send(client, W, "regtest", "not-an-address", "1").is_err());
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
        match client.call_wallet(W, method, json!([])) {
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

#[test]
fn wallets_open_close_create_and_back_up() {
    let Some((test, status)) = TestNode::start("wallets") else {
        return;
    };
    assert!(matches!(status, NodeStatus::Ready { .. }), "{status:?}");
    let client = test.node.client().unwrap();

    // No wallet yet: regtest makes its test wallet, and nothing else is listed.
    assert_eq!(
        wallet::select(client, "regtest", None).unwrap().as_deref(),
        Some(W)
    );
    let list = wallet::wallets(client).unwrap();
    assert_eq!(list.loaded, [W]);
    assert!(list.available.is_empty(), "{list:?}");

    // Create picks a plain name; a path is refused before the node sees it.
    assert!(wallet::create(client, "../outside").is_err());
    assert_eq!(wallet::create(client, " Savings ").unwrap(), "Savings");
    assert!(wallet::create(client, "Savings").is_err());
    assert_eq!(
        wallet::select(client, "regtest", Some("Savings"))
            .unwrap()
            .as_deref(),
        Some("Savings")
    );
    // A preferred wallet that is not loaded falls back to the test wallet.
    assert_eq!(
        wallet::select(client, "regtest", Some("gone"))
            .unwrap()
            .as_deref(),
        Some(W)
    );

    // Backup writes a new file and never overwrites one.
    let backup = test.datadir.join("savings-backup.dat");
    wallet::backup(client, "Savings", &backup).unwrap();
    assert!(std::fs::metadata(&backup).unwrap().len() > 0);
    assert!(wallet::backup(client, "Savings", &backup).is_err());

    // Close moves it to the folder list; open brings it back.
    wallet::close(client, "Savings").unwrap();
    let list = wallet::wallets(client).unwrap();
    assert_eq!(list.loaded, [W]);
    assert_eq!(list.available, ["Savings"]);
    assert!(wallet::open(client, "not-there").is_err());
    wallet::open(client, "Savings").unwrap();
    let list = wallet::wallets(client).unwrap();
    assert_eq!(list.loaded.len(), 2);
    assert!(list.available.is_empty());

    // Close only unloads: settings.json no longer loads it at start, and the
    // wallet stays in the folder.
    wallet::close(client, "Savings").unwrap();
    let saved = std::fs::read_to_string(test.datadir.join("regtest").join("settings.json"))
        .unwrap_or_default();
    assert!(!saved.contains("Savings"), "{saved}");
    assert!(test.datadir.join("regtest/wallets/Savings").is_dir());
}

#[test]
fn receive_makes_labelled_addresses_and_links() {
    let Some((test, status)) = TestNode::start("receive") else {
        return;
    };
    assert!(matches!(status, NodeStatus::Ready { .. }), "{status:?}");
    let client = test.node.client().unwrap();
    wallet::ensure_wallet(client).unwrap();

    let r = receive::new_request(client, W, "regtest", " rent ", "1.25", "for May").unwrap();
    assert!(r.address.starts_with("bcrt1"), "{r:?}");
    assert_eq!(r.label, "rent");
    assert_eq!(
        r.uri,
        format!(
            "b3coin:{}?amount=1.25&label=rent&message=for%20May",
            r.address.to_uppercase()
        )
    );
    assert!(r.qr.is_some());
    assert!(receive::new_request(client, W, "regtest", "", "1.2.3", "").is_err());

    // The address book lists it, and Show rebuilds its link without the
    // amount and message, which only lived in that link.
    let list = receive::addresses(client, W).unwrap();
    let mine = list.iter().find(|a| a.address == r.address).unwrap();
    assert_eq!(mine.label, "rent");
    assert_eq!(mine.transactions, 0);
    let shown = receive::show(client, W, "regtest", &r.address).unwrap();
    assert_eq!(
        shown.uri,
        format!("b3coin:{}?label=rent", r.address.to_uppercase())
    );

    // An address this wallet does not own is refused.
    let other = "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080";
    assert!(receive::show(client, W, "regtest", other).is_err());
}
