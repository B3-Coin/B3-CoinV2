//! Starts a real b3coind on regtest with no peers, waits for init, and stops it.
//! Runs only when B3_HIVE_B3COIND names the binary:
//!   B3_HIVE_B3COIND=/path/to/b3coind cargo test --test node_regtest

use std::time::Duration;

use b3_hive::node::{Node, NodeConfig, NodeStatus};

#[test]
fn starts_reports_ready_and_stops() {
    let Some(b3coind) = std::env::var_os("B3_HIVE_B3COIND") else {
        eprintln!("skipped: B3_HIVE_B3COIND is not set");
        return;
    };
    let datadir = std::env::temp_dir().join(format!("hive-node-test-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&datadir);

    let node = Node::start(&NodeConfig { b3coind: b3coind.into(), datadir: datadir.clone() });
    let mut seen = Vec::new();
    let status = node.wait_ready(Duration::from_secs(300), |s| seen.push(s.clone()));

    match &status {
        NodeStatus::Ready { chain, blocks, best_block_hash } => {
            assert_eq!(chain, "regtest");
            assert_eq!(*blocks, 0);
            assert_eq!(best_block_hash.len(), 64);
        }
        other => panic!("node did not become ready: {other:?}"),
    }
    assert!(matches!(seen.first(), Some(NodeStatus::Starting { .. }) | Some(NodeStatus::Ready { .. })));

    node.stop(Duration::from_secs(120));
    assert!(node.has_exited());
    let _ = std::fs::remove_dir_all(&datadir);
}
