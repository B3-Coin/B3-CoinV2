//! The Activity page: the wallet's whole history, filtered as Qt's
//! transaction view filters it (type, date, text, minimum amount), one
//! transaction's details, and Qt's CSV export.

use std::fmt::Write as _;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use serde::{Deserialize, Serialize};
use serde_json::{json, Value};

use crate::rpc::{Client, RpcError};
use crate::wallet::{self, ActivityEntry};

/// More lines than any page asks for; a bound on one listtransactions reply.
const HISTORY_CAP: u64 = 1_000_000;
/// Qt's RecommendedNumConfirmations.
const CONFIRMED_DEPTH: i64 = 6;

#[derive(Debug, Clone, Copy, Default, PartialEq, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Kind {
    #[default]
    All,
    Received,
    Sent,
    Mined,
    Other,
}

/// Qt's filters. Times are Unix seconds, `from` inclusive, `to` exclusive.
#[derive(Debug, Clone, Default, PartialEq, Deserialize)]
#[serde(default)]
pub struct Filter {
    pub kind: Kind,
    pub from: Option<i64>,
    pub to: Option<i64>,
    /// Part of an address, label or transaction id, any case.
    pub search: String,
    /// Exact B3 text; lines whose size is smaller are left out.
    pub min_amount: Option<String>,
}

fn kind_of(category: &str) -> Kind {
    match category {
        "receive" => Kind::Received,
        "send" => Kind::Sent,
        "generate" | "immature" | "orphan" => Kind::Mined,
        _ => Kind::Other,
    }
}

/// B3 text to base units (9 decimals), sign kept; None if it is not one.
pub fn units(text: &str) -> Option<i128> {
    let (negative, digits) = match text.strip_prefix('-') {
        Some(rest) => (true, rest),
        None => (false, text),
    };
    let (whole, frac) = digits.split_once('.').unwrap_or((digits, ""));
    let all_digits = |s: &str| s.bytes().all(|b| b.is_ascii_digit());
    if whole.is_empty()
        || !all_digits(whole)
        || !all_digits(frac)
        || frac.len() > 9
        || whole.len() > 20
    {
        return None;
    }
    let value =
        whole.parse::<i128>().ok()? * 1_000_000_000 + format!("{frac:0<9}").parse::<i128>().ok()?;
    Some(if negative { -value } else { value })
}

impl Filter {
    /// The minimum amount in base units, or why it is not an amount.
    fn minimum(&self) -> Result<Option<i128>, String> {
        match self.min_amount.as_deref().map(str::trim) {
            None | Some("") => Ok(None),
            Some(text) => match units(text) {
                Some(n) if n >= 0 => Ok(Some(n)),
                _ => Err("Enter the minimum amount like 12.5, with at most 9 decimals.".into()),
            },
        }
    }

    fn keeps(&self, e: &ActivityEntry, minimum: Option<i128>) -> bool {
        if self.kind != Kind::All && kind_of(&e.category) != self.kind {
            return false;
        }
        if self.from.is_some_and(|from| e.time < from) || self.to.is_some_and(|to| e.time >= to) {
            return false;
        }
        let needle = self.search.trim().to_lowercase();
        if !needle.is_empty()
            && ![&e.address, &e.label, &e.txid]
                .iter()
                .any(|field| field.to_lowercase().contains(&needle))
        {
            return false;
        }
        minimum.map_or(true, |min| units(&e.amount).is_some_and(|a| a.abs() >= min))
    }
}

/// Every line of the wallet's history, newest first.
pub fn all(client: &Client, wallet: &str) -> Result<Vec<ActivityEntry>, RpcError> {
    let list = client.call_wallet(wallet, "listtransactions", json!(["*", HISTORY_CAP]))?;
    let list = list
        .as_array()
        .ok_or_else(|| RpcError::BadReply("listtransactions did not return a list".into()))?;
    list.iter()
        .rev()
        .map(wallet::parse_activity)
        .collect::<Result<Vec<_>, _>>()
        .map(wallet::settle_flowmesh)
}

/// What a cached history depends on: a new block changes every line's
/// confirmations, a new wallet transaction adds lines.
#[derive(Debug, Clone, PartialEq)]
struct Key {
    wallet: String,
    tip: String,
    txcount: u64,
}

/// Longest a cached history is reused even when its key still matches, for
/// what the key cannot see (an abandoned transaction, a changed label).
const CACHE_TTL: Duration = Duration::from_secs(15);

/// The last full history read. The Activity page asks again on every
/// filter change and keystroke; reading the whole history each time is
/// slow on a large wallet and holds the wallet meanwhile.
#[derive(Default)]
pub struct Cache(Mutex<Option<Cached>>);

type Cached = (Key, Instant, Arc<Vec<ActivityEntry>>);

impl Cache {
    /// Forgets the cached history, after something it holds changed, such
    /// as an address label.
    pub fn clear(&self) {
        self.0.lock().unwrap().take();
    }

    /// The wallet's whole history, newest first: the cached one when the
    /// tip and the wallet's transaction count are unchanged, else read anew.
    pub fn all(&self, client: &Client, wallet: &str) -> Result<Arc<Vec<ActivityEntry>>, RpcError> {
        let tip = client.call("getbestblockhash", json!([]))?;
        let info = client.call_wallet(wallet, "getwalletinfo", json!([]))?;
        let key = Key {
            wallet: wallet.to_owned(),
            tip: tip.as_str().unwrap_or_default().to_owned(),
            txcount: info["txcount"].as_u64().unwrap_or(u64::MAX),
        };
        if let Some((cached, at, lines)) = &*self.0.lock().unwrap() {
            if *cached == key && at.elapsed() < CACHE_TTL {
                return Ok(Arc::clone(lines));
            }
        }
        let lines = Arc::new(all(client, wallet)?);
        *self.0.lock().unwrap() = Some((key, Instant::now(), Arc::clone(&lines)));
        Ok(lines)
    }
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Page {
    /// The newest `count` matching lines.
    pub entries: Vec<ActivityEntry>,
    /// Lines that match the filter.
    pub matching: usize,
    /// Lines in the wallet.
    pub total: usize,
}

pub fn filter(lines: &[ActivityEntry], filter: &Filter, count: usize) -> Result<Page, String> {
    let minimum = filter.minimum()?;
    let mut matching = 0;
    let mut entries = Vec::new();
    for line in lines.iter().filter(|e| filter.keeps(e, minimum)) {
        matching += 1;
        if entries.len() < count {
            entries.push(line.clone());
        }
    }
    Ok(Page {
        entries,
        matching,
        total: lines.len(),
    })
}

/// Unix seconds as UTC "YYYY-MM-DDTHH:MM:SS" (Howard Hinnant's civil date).
pub(crate) fn utc(seconds: i64) -> String {
    let days = seconds.div_euclid(86_400);
    let secs = seconds.rem_euclid(86_400);
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = yoe + era * 400 + i64::from(month <= 2);
    format!(
        "{year:04}-{month:02}-{day:02}T{:02}:{:02}:{:02}",
        secs / 3600,
        secs / 60 % 60,
        secs % 60
    )
}

fn csv_field(text: &str) -> String {
    format!("\"{}\"", text.replace('"', "\"\""))
}

fn type_text(category: &str) -> &str {
    match category {
        "receive" => "Received with",
        "send" => "Sent to",
        "generate" | "immature" | "orphan" => "Mined",
        other => other,
    }
}

/// Qt's export columns, with the date in UTC and said so.
pub fn csv(lines: &[ActivityEntry]) -> String {
    let mut out = String::from(
        "\"Confirmed\",\"Date (UTC)\",\"Type\",\"Label\",\"Address\",\"Amount (B3)\",\"ID\"\n",
    );
    for e in lines {
        let confirmed = match e.category.as_str() {
            "immature" | "orphan" => false,
            _ => e.confirmations >= CONFIRMED_DEPTH,
        };
        let _ = writeln!(
            out,
            "{},{},{},{},{},{},{}",
            csv_field(if confirmed { "true" } else { "false" }),
            csv_field(&utc(e.time)),
            csv_field(type_text(&e.category)),
            csv_field(&e.label),
            csv_field(&e.address),
            e.amount,
            csv_field(&e.txid),
        );
    }
    out
}

/// One output line of a transaction, as gettransaction's details list it.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct DetailLine {
    pub category: String,
    pub address: String,
    pub label: String,
    pub amount: String,
    pub vout: i64,
}

/// What Qt's transaction details dialog shows.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Details {
    pub txid: String,
    pub wtxid: String,
    /// Net change to the wallet, signed.
    pub amount: String,
    pub fee: Option<String>,
    pub confirmations: i64,
    pub generated: bool,
    pub abandoned: bool,
    pub block_hash: Option<String>,
    pub block_height: Option<i64>,
    pub time: i64,
    pub time_received: i64,
    pub comment: String,
    pub comment_to: String,
    /// Bytes.
    pub size: usize,
    pub lines: Vec<DetailLine>,
}

fn is_txid(text: &str) -> bool {
    text.len() == 64 && text.bytes().all(|b| b.is_ascii_hexdigit())
}

pub fn parse_details(t: &Value) -> Result<Details, RpcError> {
    let text = |v: &Value| v.as_str().unwrap_or_default().to_owned();
    let lines = t["details"]
        .as_array()
        .map(Vec::as_slice)
        .unwrap_or_default()
        .iter()
        .map(|d| {
            Ok(DetailLine {
                category: text(&d["category"]),
                address: text(&d["address"]),
                label: text(&d["label"]),
                amount: wallet::decimal(&d["amount"])?,
                vout: d["vout"].as_i64().unwrap_or(-1),
            })
        })
        .collect::<Result<Vec<_>, RpcError>>()?;
    if t["b3"].as_str().is_some_and(wallet::vault_tx) {
        return Ok(payout_details(t, lines, details_of(t, Vec::new())?));
    }
    details_of(t, lines)
}

/// A payout or deposit sweep as what it did to this wallet (see
/// wallet::settle_flowmesh): what it received, and the fee it paid. A payout
/// to another wallet lists where it went.
fn payout_details(t: &Value, lines: Vec<DetailLine>, mut d: Details) -> Details {
    let received: Vec<DetailLine> = lines
        .iter()
        .filter(|l| l.category == "receive")
        .cloned()
        .collect();
    d.amount = wallet::b3_text(received.iter().filter_map(|l| units(&l.amount)).sum());
    d.fee = t["b3_network_fee"].as_str().and_then(wallet::tagged_fee);
    d.lines = if received.is_empty() {
        lines
            .into_iter()
            .filter(|l| !l.address.is_empty())
            .collect()
    } else {
        received
    };
    d
}

fn details_of(t: &Value, lines: Vec<DetailLine>) -> Result<Details, RpcError> {
    let text = |v: &Value| v.as_str().unwrap_or_default().to_owned();
    Ok(Details {
        txid: text(&t["txid"]),
        wtxid: text(&t["wtxid"]),
        amount: wallet::decimal(&t["amount"])?,
        fee: match &t["fee"] {
            Value::Null => None,
            fee => Some(wallet::decimal(fee)?),
        },
        confirmations: t["confirmations"].as_i64().unwrap_or(0),
        generated: t["generated"].as_bool().unwrap_or(false),
        abandoned: t["details"]
            .as_array()
            .is_some_and(|d| d.iter().any(|l| l["abandoned"] == Value::Bool(true))),
        block_hash: t["blockhash"].as_str().map(str::to_owned),
        block_height: t["blockheight"].as_i64(),
        time: t["time"].as_i64().unwrap_or(0),
        time_received: t["timereceived"].as_i64().unwrap_or(0),
        comment: text(&t["comment"]),
        comment_to: text(&t["to"]),
        size: t["hex"].as_str().map_or(0, |h| h.len() / 2),
        lines,
    })
}

pub fn details(client: &Client, wallet: &str, txid: &str) -> Result<Details, String> {
    if !is_txid(txid) {
        return Err("That is not a transaction id.".into());
    }
    let t = client
        .call_wallet(wallet, "gettransaction", json!([txid]))
        .map_err(|e| e.to_string())?;
    parse_details(&t).map_err(|e| e.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn line(category: &str, amount: &str, time: i64, address: &str, label: &str) -> ActivityEntry {
        ActivityEntry {
            txid: format!("{:064x}", time),
            category: category.into(),
            amount: amount.into(),
            fee: None,
            confirmations: 10,
            time,
            address: address.into(),
            label: label.into(),
            tag: String::new(),
            network_fee: None,
        }
    }

    fn sample() -> Vec<ActivityEntry> {
        vec![
            line("receive", "2.500000000", 400, "b3qalice", "Alice"),
            line("send", "-0.100000000", 300, "b3qbob", ""),
            line("immature", "0.050000000", 200, "b3qmine", ""),
            line("generate", "0.050000000", 100, "b3qmine", ""),
        ]
    }

    fn times(f: &Filter, count: usize) -> (Vec<i64>, usize) {
        let page = filter(&sample(), f, count).unwrap();
        assert_eq!(page.total, 4);
        (page.entries.iter().map(|e| e.time).collect(), page.matching)
    }

    #[test]
    fn units_are_exact() {
        assert_eq!(units("2.5"), Some(2_500_000_000));
        assert_eq!(units("-0.000000001"), Some(-1));
        assert_eq!(units("12"), Some(12_000_000_000));
        for bad in ["", ".5", "1.0000000001", "1e5", "--1", "1.2.3", "abc"] {
            assert_eq!(units(bad), None, "{bad}");
        }
    }

    #[test]
    fn filters_match_qt() {
        let all = Filter::default();
        assert_eq!(times(&all, 10), (vec![400, 300, 200, 100], 4));
        assert_eq!(times(&all, 2), (vec![400, 300], 4));
        let kind = |kind| Filter {
            kind,
            ..Default::default()
        };
        assert_eq!(times(&kind(Kind::Mined), 10).0, [200, 100]);
        assert_eq!(times(&kind(Kind::Sent), 10).0, [300]);
        assert_eq!(times(&kind(Kind::Other), 10).1, 0);
        let range = Filter {
            from: Some(200),
            to: Some(400),
            ..Default::default()
        };
        assert_eq!(times(&range, 10).0, [300, 200]);
        let search = |s: &str| Filter {
            search: s.into(),
            ..Default::default()
        };
        assert_eq!(times(&search("ALICE"), 10).0, [400]);
        assert_eq!(times(&search("b3qbob"), 10).0, [300]);
        assert_eq!(times(&search(&format!("{:x}", 200)), 10).0, [200]);
        // Sends count by size, as Qt compares absolute amounts.
        let min = Filter {
            min_amount: Some("0.1".into()),
            ..Default::default()
        };
        assert_eq!(times(&min, 10).0, [400, 300]);
        let bad = Filter {
            min_amount: Some("lots".into()),
            ..Default::default()
        };
        assert!(filter(&sample(), &bad, 10).is_err());
    }

    #[test]
    fn csv_has_qt_columns_and_quotes() {
        let mut lines = sample();
        lines[0].label = "Al \"the\" ice".into();
        lines[0].time = 1_760_745_600; // 2025-10-18T00:00:00Z
        let text = csv(&lines);
        let rows: Vec<_> = text.lines().collect();
        assert_eq!(
            rows[0],
            "\"Confirmed\",\"Date (UTC)\",\"Type\",\"Label\",\"Address\",\"Amount (B3)\",\"ID\""
        );
        assert!(
            rows[1].starts_with(
                "\"true\",\"2025-10-18T00:00:00\",\"Received with\",\"Al \"\"the\"\" ice\",\"b3qalice\",2.500000000,"
            ),
            "{}",
            rows[1]
        );
        assert!(rows[2].contains("\"Sent to\""), "{}", rows[2]);
        // Maturing coins are not confirmed however deep.
        assert!(rows[3].starts_with("\"false\""), "{}", rows[3]);
        assert_eq!(utc(0), "1970-01-01T00:00:00");
        assert_eq!(utc(951_782_400), "2000-02-29T00:00:00");
    }

    #[test]
    fn details_read_gettransaction() {
        let d = parse_details(&json!({
            "txid": "aa", "wtxid": "bb", "amount": -0.1, "fee": -0.0001,
            "confirmations": 3, "blockhash": "cc", "blockheight": 120, "time": 5,
            "timereceived": 6, "comment": "rent", "hex": "00112233",
            "details": [{"category": "send", "address": "b3qbob", "amount": -0.1,
                         "vout": 1, "abandoned": false}]
        }))
        .unwrap();
        assert_eq!(d.size, 4);
        assert_eq!(d.block_height, Some(120));
        assert_eq!(d.lines.len(), 1);
        assert!(!d.abandoned && !d.generated);
        assert_eq!(d.comment, "rent");
        assert!(!is_txid("zz"));
        assert!(is_txid(&"Ab".repeat(32)));
    }

    /// Lines as the node lists a payout to this wallet and a checkpoint
    /// (regtest), oldest first.
    fn flowmesh_lines() -> Value {
        let payout = |category: &str, amount: f64, fee: Value, address: Value, vout: u64| {
            json!({"txid": "c6".repeat(32), "category": category, "amount": amount, "fee": fee,
                   "address": address, "label": if address.is_null() { "" } else { "mine" },
                   "vout": vout, "b3": "flowmesh-withdrawal", "b3_network_fee": "0.0003954",
                   "time": 9, "confirmations": 1})
        };
        let checkpoint = |category: &str, amount: f64, fee: Value| {
            json!({"txid": "a2".repeat(32), "category": category, "amount": amount, "fee": fee,
                   "address": "mqv9", "label": "b3-flowmesh-checkpoint", "vout": 0,
                   "time": 8, "confirmations": 2})
        };
        json!([
            checkpoint("receive", 0.000000546, Value::Null),
            checkpoint("send", -0.000000546, json!(-0.0036)),
            payout("receive", 0.01, Value::Null, json!("mkyB"), 0),
            payout("send", -0.99, json!(0.9996046), Value::Null, 1),
            payout("send", -0.01, json!(0.9996046), json!("mkyB"), 0),
        ])
    }

    #[test]
    fn flowmesh_lines_show_what_they_did_to_the_wallet() {
        let raw = flowmesh_lines();
        let lines: Vec<ActivityEntry> = raw
            .as_array()
            .unwrap()
            .iter()
            .rev()
            .map(|l| wallet::parse_activity(l).unwrap())
            .collect();
        let got = wallet::settle_flowmesh(lines);
        assert_eq!(got.len(), 2);
        // The payout: what the wallet received, and the fee it paid.
        assert_eq!(got[0].category, "receive");
        assert_eq!(got[0].amount, "0.01");
        assert_eq!(got[0].fee.as_deref(), Some("-0.000395400"));
        assert_eq!(got[0].label, "mine");
        // The checkpoint: one line, its fee, nothing sent away.
        assert_eq!(got[1].category, "send");
        assert_eq!(got[1].amount, "0.000000000");
        assert_eq!(got[1].fee.as_deref(), Some("-0.0036"));
        assert_eq!(got[1].label, "FlowMesh checkpoint");

        // Paid to another wallet: the fee only, and where it went.
        let elsewhere: Vec<ActivityEntry> = raw.as_array().unwrap()[3..]
            .iter()
            .map(|l| wallet::parse_activity(l).unwrap())
            .collect();
        let got = wallet::settle_flowmesh(elsewhere);
        assert_eq!(got.len(), 1);
        assert_eq!(
            (got[0].category.as_str(), got[0].amount.as_str()),
            ("send", "0.000000000")
        );
        assert_eq!(
            (got[0].address.as_str(), got[0].fee.as_deref()),
            ("mkyB", Some("-0.000395400"))
        );

        // A deposit sweep this wallet published: the fee only. A deposit
        // keeps the node's line and gets a name.
        let sweep = json!({"txid": "b9".repeat(32), "category": "send", "amount": -1.0,
                           "fee": 0.99999403, "vout": 0, "b3": "flowmesh-deposit-sweep",
                           "b3_network_fee": "0.00000597"});
        let deposit = json!({"txid": "8e".repeat(32), "category": "send", "amount": -1.0,
                             "fee": -0.00000319, "vout": 0, "b3": "flowmesh-deposit"});
        let got = wallet::settle_flowmesh(vec![
            wallet::parse_activity(&sweep).unwrap(),
            wallet::parse_activity(&deposit).unwrap(),
        ]);
        assert_eq!(
            (got[0].amount.as_str(), got[0].fee.as_deref()),
            ("0.000000000", Some("-0.000005970"))
        );
        assert_eq!(got[0].label, "FlowMesh deposit sweep");
        assert_eq!(got[1].fee, wallet::parse_activity(&deposit).unwrap().fee);
        assert_eq!(got[1].label, "FlowMesh deposit");
    }

    #[test]
    fn payout_details_show_what_the_wallet_received() {
        let d = parse_details(&json!({
            "txid": "c6", "amount": -0.99, "fee": 0.9996046, "confirmations": 1,
            "b3": "flowmesh-withdrawal", "b3_network_fee": "0.0003954",
            "details": [
                {"category": "send", "address": "mkyB", "amount": -0.01, "vout": 0},
                {"category": "send", "amount": -0.99, "vout": 1},
                {"category": "receive", "address": "mkyB", "amount": 0.01, "vout": 0},
            ]
        }))
        .unwrap();
        assert_eq!(d.amount, "0.010000000");
        assert_eq!(d.fee.as_deref(), Some("-0.000395400"));
        assert_eq!(d.lines.len(), 1);
        assert_eq!(d.lines[0].category, "receive");
    }
}
