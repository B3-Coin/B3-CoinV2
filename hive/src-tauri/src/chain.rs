//! Read-only node pages: the dashboard's chain, sync and network summary and
//! the network page's peer list. Each maps a few read RPCs one to one onto a
//! typed reply; nothing here changes node state.

use serde::Serialize;
use serde_json::{json, Value};

use crate::rpc::{Client, RpcError};

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct ChainOverview {
    pub chain: String,
    pub blocks: u64,
    pub headers: u64,
    pub best_block_hash: String,
    /// 0 to 1, as the node estimates it.
    pub verification_progress: f64,
    pub initial_block_download: bool,
    /// Median time of the last 11 blocks, in Unix seconds.
    pub median_time: i64,
    pub size_on_disk: u64,
    pub pruned: bool,
    pub user_agent: String,
    pub protocol_version: i64,
    pub connections_in: u64,
    pub connections_out: u64,
    pub network_active: bool,
    pub mempool_transactions: u64,
    pub mempool_bytes: u64,
    /// Chain and network warnings, as the node words them.
    pub warnings: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Peer {
    pub id: i64,
    pub address: String,
    pub inbound: bool,
    pub connection_type: String,
    pub user_agent: String,
    pub version: i64,
    /// -1 while the node does not know the peer's tip.
    pub synced_blocks: i64,
    /// None until the first ping is answered.
    pub ping_ms: Option<f64>,
    pub bytes_sent: u64,
    pub bytes_received: u64,
    /// Unix seconds.
    pub connected_since: i64,
    /// The rest of Qt's peer details.
    pub network: String,
    pub services: Vec<String>,
    pub permissions: Vec<String>,
    pub transport: String,
    pub session_id: String,
    pub relays_transactions: bool,
    pub high_bandwidth_to: bool,
    pub high_bandwidth_from: bool,
    /// -1 when unknown.
    pub synced_headers: i64,
    /// Unix seconds; 0 for never.
    pub last_send: i64,
    pub last_receive: i64,
    pub last_block: i64,
    pub last_transaction: i64,
    pub min_ping_ms: Option<f64>,
    pub ping_wait_ms: Option<f64>,
    pub time_offset: i64,
    pub mapped_as: Option<i64>,
}

/// Qt's node window Information tab, besides what ChainOverview has.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct NodeDetails {
    /// Seconds since b3coind started.
    pub uptime: u64,
    /// Addresses the node announces, as address:port.
    pub local_addresses: Vec<String>,
    pub mempool_usage: u64,
    pub mempool_max: u64,
    pub total_received: u64,
    pub total_sent: u64,
    pub banned: Vec<Ban>,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Ban {
    pub address: String,
    pub created: i64,
    pub until: i64,
}

pub fn overview(client: &Client) -> Result<ChainOverview, RpcError> {
    let chain = client.call("getblockchaininfo", json!([]))?;
    let network = client.call("getnetworkinfo", json!([]))?;
    let mempool = client.call("getmempoolinfo", json!([]))?;
    Ok(parse_overview(&chain, &network, &mempool))
}

pub fn details(client: &Client) -> Result<NodeDetails, RpcError> {
    let uptime = client.call("uptime", json!([]))?;
    let network = client.call("getnetworkinfo", json!([]))?;
    let mempool = client.call("getmempoolinfo", json!([]))?;
    let totals = client.call("getnettotals", json!([]))?;
    let banned = client.call("listbanned", json!([]))?;
    Ok(parse_details(&uptime, &network, &mempool, &totals, &banned))
}

fn parse_details(
    uptime: &Value,
    network: &Value,
    mempool: &Value,
    totals: &Value,
    banned: &Value,
) -> NodeDetails {
    let list = |v: &Value| v.as_array().cloned().unwrap_or_default();
    NodeDetails {
        uptime: count(uptime),
        local_addresses: list(&network["localaddresses"])
            .iter()
            .map(|a| {
                let host = text(&a["address"]);
                // IPv6 in brackets, as b3coind takes it back.
                let host = if host.contains(':') {
                    format!("[{host}]")
                } else {
                    host
                };
                format!("{host}:{}", count(&a["port"]))
            })
            .collect(),
        mempool_usage: count(&mempool["usage"]),
        mempool_max: count(&mempool["maxmempool"]),
        total_received: count(&totals["totalbytesrecv"]),
        total_sent: count(&totals["totalbytessent"]),
        banned: list(banned)
            .iter()
            .map(|b| Ban {
                address: text(&b["address"]),
                created: int(&b["ban_created"], 0),
                until: int(&b["banned_until"], 0),
            })
            .collect(),
    }
}

pub fn peers(client: &Client) -> Result<Vec<Peer>, RpcError> {
    let info = client.call("getpeerinfo", json!([]))?;
    let list = info
        .as_array()
        .ok_or_else(|| RpcError::BadReply("getpeerinfo did not return a list".into()))?;
    Ok(list.iter().map(parse_peer).collect())
}

fn text(v: &Value) -> String {
    v.as_str().unwrap_or_default().to_owned()
}

fn strings(v: &Value) -> Vec<String> {
    v.as_array()
        .map(|items| {
            items
                .iter()
                .filter_map(Value::as_str)
                .map(str::to_owned)
                .collect()
        })
        .unwrap_or_default()
}

fn count(v: &Value) -> u64 {
    v.as_u64().unwrap_or(0)
}

fn int(v: &Value, missing: i64) -> i64 {
    v.as_i64().unwrap_or(missing)
}

/// The warnings of both RPCs, without repeats: each reports the shared ones.
fn warnings(sources: [&Value; 2]) -> Vec<String> {
    let mut all: Vec<String> = Vec::new();
    for source in sources {
        let items = match source {
            Value::Array(items) => items.iter().filter_map(Value::as_str).collect(),
            // With -deprecatedrpc=warnings the node returns one string.
            Value::String(s) => vec![s.as_str()],
            _ => vec![],
        };
        for w in items {
            if !w.is_empty() && !all.iter().any(|seen| seen == w) {
                all.push(w.to_owned());
            }
        }
    }
    all
}

fn parse_overview(chain: &Value, network: &Value, mempool: &Value) -> ChainOverview {
    ChainOverview {
        chain: text(&chain["chain"]),
        blocks: count(&chain["blocks"]),
        headers: count(&chain["headers"]),
        best_block_hash: text(&chain["bestblockhash"]),
        verification_progress: chain["verificationprogress"]
            .as_f64()
            .unwrap_or(0.0)
            .clamp(0.0, 1.0),
        initial_block_download: chain["initialblockdownload"].as_bool().unwrap_or(false),
        median_time: int(&chain["mediantime"], 0),
        size_on_disk: count(&chain["size_on_disk"]),
        pruned: chain["pruned"].as_bool().unwrap_or(false),
        user_agent: text(&network["subversion"]),
        protocol_version: int(&network["protocolversion"], 0),
        connections_in: count(&network["connections_in"]),
        connections_out: count(&network["connections_out"]),
        network_active: network["networkactive"].as_bool().unwrap_or(false),
        mempool_transactions: count(&mempool["size"]),
        mempool_bytes: count(&mempool["bytes"]),
        warnings: warnings([&chain["warnings"], &network["warnings"]]),
    }
}

fn parse_peer(p: &Value) -> Peer {
    Peer {
        id: int(&p["id"], -1),
        address: text(&p["addr"]),
        inbound: p["inbound"].as_bool().unwrap_or(false),
        connection_type: text(&p["connection_type"]),
        user_agent: text(&p["subver"]),
        version: int(&p["version"], 0),
        synced_blocks: int(&p["synced_blocks"], -1),
        ping_ms: p["pingtime"].as_f64().map(|s| s * 1000.0),
        bytes_sent: count(&p["bytessent"]),
        bytes_received: count(&p["bytesrecv"]),
        connected_since: int(&p["conntime"], 0),
        network: text(&p["network"]),
        services: strings(&p["servicesnames"]),
        permissions: strings(&p["permissions"]),
        transport: text(&p["transport_protocol_type"]),
        session_id: text(&p["session_id"]),
        relays_transactions: p["relaytxes"].as_bool().unwrap_or(false),
        high_bandwidth_to: p["bip152_hb_to"].as_bool().unwrap_or(false),
        high_bandwidth_from: p["bip152_hb_from"].as_bool().unwrap_or(false),
        synced_headers: int(&p["synced_headers"], -1),
        last_send: int(&p["lastsend"], 0),
        last_receive: int(&p["lastrecv"], 0),
        last_block: int(&p["last_block"], 0),
        last_transaction: int(&p["last_transaction"], 0),
        min_ping_ms: p["minping"].as_f64().map(|s| s * 1000.0),
        ping_wait_ms: p["pingwait"].as_f64().map(|s| s * 1000.0),
        time_offset: int(&p["timeoffset"], 0),
        mapped_as: p["mapped_as"].as_i64(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn details_read_qt_information_tab() {
        let d = parse_details(
            &json!(3600),
            &json!({"localaddresses": [
                {"address": "203.0.113.5", "port": 5647, "score": 1},
                {"address": "2001:db8::1", "port": 5647, "score": 1}
            ]}),
            &json!({"usage": 2048, "maxmempool": 300000000}),
            &json!({"totalbytesrecv": 10, "totalbytessent": 20}),
            &json!([{"address": "198.51.100.0/24", "ban_created": 5, "banned_until": 9}]),
        );
        assert_eq!(d.uptime, 3600);
        assert_eq!(
            d.local_addresses,
            ["203.0.113.5:5647", "[2001:db8::1]:5647"]
        );
        assert_eq!((d.mempool_usage, d.mempool_max), (2048, 300000000));
        assert_eq!((d.total_received, d.total_sent), (10, 20));
        assert_eq!(
            d.banned,
            [Ban {
                address: "198.51.100.0/24".into(),
                created: 5,
                until: 9
            }]
        );
    }

    #[test]
    fn peers_carry_qt_details() {
        let p = parse_peer(&json!({
            "id": 7, "addr": "203.0.113.9:5647", "network": "ipv4",
            "servicesnames": ["NETWORK", "WITNESS"], "permissions": ["noban"],
            "transport_protocol_type": "v2", "session_id": "ab", "relaytxes": true,
            "bip152_hb_to": true, "synced_headers": 900, "lastsend": 11, "lastrecv": 12,
            "last_block": 13, "last_transaction": 14, "minping": 0.02, "pingwait": 0.5,
            "timeoffset": -2, "mapped_as": 64496
        }));
        assert_eq!(p.services, ["NETWORK", "WITNESS"]);
        assert_eq!(p.permissions, ["noban"]);
        assert!(p.relays_transactions && p.high_bandwidth_to && !p.high_bandwidth_from);
        assert_eq!(
            (p.synced_headers, p.last_block, p.time_offset),
            (900, 13, -2)
        );
        assert_eq!(p.min_ping_ms, Some(20.0));
        assert_eq!(p.mapped_as, Some(64496));
        let bare = parse_peer(&json!({}));
        assert_eq!(
            (bare.synced_headers, bare.mapped_as, bare.ping_wait_ms),
            (-1, None, None)
        );
    }

    #[test]
    fn overview_reads_the_three_rpcs() {
        let chain = json!({
            "chain": "main", "blocks": 120, "headers": 150,
            "bestblockhash": "ab", "verificationprogress": 0.75,
            "initialblockdownload": true, "mediantime": 1700000000,
            "size_on_disk": 4096, "pruned": false,
            "warnings": ["Unknown new rules activated"]
        });
        let network = json!({
            "subversion": "/B3Coin:1.1.5/", "protocolversion": 70016,
            "connections_in": 2, "connections_out": 8, "networkactive": true,
            "warnings": ["Unknown new rules activated", "Clock is off"]
        });
        let mempool = json!({"size": 3, "bytes": 900});
        let o = parse_overview(&chain, &network, &mempool);
        assert_eq!(o.chain, "main");
        assert_eq!((o.blocks, o.headers), (120, 150));
        assert_eq!(o.verification_progress, 0.75);
        assert!(o.initial_block_download);
        assert_eq!((o.connections_in, o.connections_out), (2, 8));
        assert_eq!((o.mempool_transactions, o.mempool_bytes), (3, 900));
        assert_eq!(o.warnings, ["Unknown new rules activated", "Clock is off"]);
    }

    #[test]
    fn overview_tolerates_missing_fields_and_one_string_warnings() {
        let o = parse_overview(
            &json!({"verificationprogress": 1.0000002, "warnings": ""}),
            &json!({"warnings": "Clock is off"}),
            &json!({}),
        );
        assert_eq!(o.verification_progress, 1.0);
        assert_eq!(o.blocks, 0);
        assert_eq!(o.warnings, ["Clock is off"]);
    }

    #[test]
    fn peers_keep_unknowns_distinct() {
        let p = parse_peer(&json!({
            "id": 4, "addr": "10.0.0.2:5647", "inbound": false,
            "connection_type": "outbound-full-relay", "subver": "/B3Coin:1.1.5/",
            "version": 70016, "synced_blocks": -1, "bytessent": 10,
            "bytesrecv": 20, "conntime": 1700000000
        }));
        assert_eq!(p.synced_blocks, -1);
        assert_eq!(p.ping_ms, None);
        let p = parse_peer(&json!({"pingtime": 0.0125}));
        assert_eq!(p.ping_ms, Some(12.5));
    }
}
