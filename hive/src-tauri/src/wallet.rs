//! Wallet calls for the spike: one regtest wallet, its balance, test funding,
//! and building a signed send for the approval window.

use std::sync::Mutex;

use serde::Serialize;
use serde_json::{json, Value};

use crate::drafts::{OutputLine, SendSummary};
use crate::rpc::{Client, RpcError};

pub const SPIKE_WALLET: &str = "hive-spike";
/// Blocks mined by the regtest funding button. Coinbase outputs mature after
/// 30 blocks and regtest pays 0.05 B3 per block, so this leaves 5 B3 spendable.
pub const FUND_BLOCKS: u64 = 130;

const RPC_WALLET_NOT_FOUND: i64 = -18;
const RPC_WALLET_ALREADY_LOADED: i64 = -35;

/// Pages ask for the wallet at the same time; two loadwallet calls racing
/// make the second fail on the database lock instead of "already loaded".
static LOADING: Mutex<()> = Mutex::new(());

/// Loads the spike wallet, creating it on first use.
pub fn ensure_wallet(client: &Client) -> Result<(), RpcError> {
    let _one_at_a_time = LOADING.lock().unwrap_or_else(|e| e.into_inner());
    let loaded = client.call("listwallets", json!([]))?;
    if loaded
        .as_array()
        .is_some_and(|w| w.iter().any(|n| n.as_str() == Some(SPIKE_WALLET)))
    {
        return Ok(());
    }
    match client.call("loadwallet", json!([SPIKE_WALLET])) {
        Ok(_)
        | Err(RpcError::Rpc {
            code: RPC_WALLET_ALREADY_LOADED,
            ..
        }) => Ok(()),
        Err(RpcError::Rpc {
            code: RPC_WALLET_NOT_FOUND,
            ..
        }) => client
            .call("createwallet", json!([SPIKE_WALLET]))
            .map(|_| ()),
        Err(e) => Err(e),
    }
}

/// Spendable and immature balance as exact decimal strings.
pub fn balances(client: &Client) -> Result<(String, String), RpcError> {
    let b = client.call_wallet(SPIKE_WALLET, "getbalances", json!([]))?;
    let mine = &b["mine"];
    Ok((decimal(&mine["trusted"])?, decimal(&mine["immature"])?))
}

/// Regtest only: mines FUND_BLOCKS blocks to a fresh wallet address.
pub fn fund_regtest(client: &Client) -> Result<(), RpcError> {
    let address = client.call_wallet(SPIKE_WALLET, "getnewaddress", json!([]))?;
    client
        .call("generatetoaddress", json!([FUND_BLOCKS, address]))
        .map(|_| ())
}

/// Accepts a positive B3 amount with at most 9 decimal places, exactly as
/// typed: no rounding, no floats. Returns it unchanged for the RPC call.
pub fn parse_amount(input: &str) -> Result<String, String> {
    let (whole, frac) = match input.split_once('.') {
        Some((w, f)) => (w, Some(f)),
        None => (input, None),
    };
    let digits = |s: &str| !s.is_empty() && s.bytes().all(|b| b.is_ascii_digit());
    if !digits(whole) || frac.is_some_and(|f| !digits(f)) {
        return Err("Enter an amount like 12.5, using digits and one decimal point.".into());
    }
    if frac.is_some_and(|f| f.len() > 9) {
        return Err("B3 amounts have at most 9 decimal places.".into());
    }
    if whole.len() > 10 {
        return Err("That amount is too large.".into());
    }
    if input.bytes().all(|b| b == b'0' || b == b'.') {
        return Err("Enter an amount greater than zero.".into());
    }
    Ok(input.to_owned())
}

/// Funds, signs and finalizes a send to `address`, then decodes the result.
/// Nothing is broadcast: the caller stores the hex as a draft.
pub fn prepare_send(
    client: &Client,
    chain: &str,
    address: &str,
    amount: &str,
) -> Result<(SendSummary, String), String> {
    let amount = parse_amount(amount)?;
    let address = address.trim();
    if address.is_empty() {
        return Err("Enter a destination address.".into());
    }
    let call = |method: &str, params: Value| {
        client
            .call_wallet(SPIKE_WALLET, method, params)
            .map_err(|e| e.to_string())
    };

    // Amount goes over the wire as a string, so the node parses the exact digits.
    let mut output = serde_json::Map::new();
    output.insert(address.to_owned(), Value::String(amount));
    let funded = call("walletcreatefundedpsbt", json!([[], [output], 0, {}]))?;
    let change_pos = funded["changepos"].as_i64().unwrap_or(-1);
    let fee = decimal(&funded["fee"]).map_err(|e| e.to_string())?;
    let psbt = funded["psbt"].as_str().ok_or("funding returned no PSBT")?;

    let signed = call("walletprocesspsbt", json!([psbt, true]))?;
    let psbt = signed["psbt"].as_str().ok_or("signing returned no PSBT")?;
    let finalized = client
        .call("finalizepsbt", json!([psbt]))
        .map_err(|e| e.to_string())?;
    if finalized["complete"].as_bool() != Some(true) {
        return Err("The wallet could not sign every input.".into());
    }
    let hex = finalized["hex"]
        .as_str()
        .ok_or("finalizing returned no transaction")?
        .to_owned();

    let decoded = client
        .call("decoderawtransaction", json!([hex]))
        .map_err(|e| e.to_string())?;
    let txid = decoded["txid"]
        .as_str()
        .ok_or("decoded transaction has no txid")?
        .to_owned();
    let mut outputs = Vec::new();
    for out in decoded["vout"]
        .as_array()
        .ok_or("decoded transaction has no outputs")?
    {
        let n = out["n"].as_i64().unwrap_or(-2);
        outputs.push(OutputLine {
            address: out["scriptPubKey"]["address"]
                .as_str()
                .unwrap_or("(no address)")
                .to_owned(),
            amount: decimal(&out["value"]).map_err(|e| e.to_string())?,
            change: n == change_pos,
        });
    }
    Ok((
        SendSummary {
            chain: chain.to_owned(),
            wallet: SPIKE_WALLET.to_owned(),
            txid,
            outputs,
            fee,
        },
        hex,
    ))
}

/// One wallet transaction line, as listtransactions reports it. A payment
/// to several addresses gives one line per address.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct ActivityEntry {
    pub txid: String,
    /// send, receive, generate, immature or orphan.
    pub category: String,
    /// Signed, exact decimal text: negative for sends.
    pub amount: String,
    /// Sends only; negative, as the node reports it.
    pub fee: Option<String>,
    /// Negative when the transaction conflicts with the chain.
    pub confirmations: i64,
    /// Unix seconds.
    pub time: i64,
    pub address: String,
    pub label: String,
}

/// Activity shows at most this many lines.
pub const ACTIVITY_LIMIT: u64 = 500;

/// The newest `count` (at most ACTIVITY_LIMIT) lines, newest first.
pub fn activity(client: &Client, count: u64) -> Result<Vec<ActivityEntry>, RpcError> {
    let count = count.clamp(1, ACTIVITY_LIMIT);
    let list = client.call_wallet(SPIKE_WALLET, "listtransactions", json!(["*", count]))?;
    let list = list
        .as_array()
        .ok_or_else(|| RpcError::BadReply("listtransactions did not return a list".into()))?;
    // The node lists oldest first.
    list.iter().rev().map(parse_activity).collect()
}

fn parse_activity(t: &Value) -> Result<ActivityEntry, RpcError> {
    let text = |v: &Value| v.as_str().unwrap_or_default().to_owned();
    Ok(ActivityEntry {
        txid: text(&t["txid"]),
        category: text(&t["category"]),
        amount: decimal(&t["amount"])?,
        fee: match &t["fee"] {
            Value::Null => None,
            fee => Some(decimal(fee)?),
        },
        confirmations: t["confirmations"].as_i64().unwrap_or(0),
        time: t["time"].as_i64().unwrap_or(0),
        address: text(&t["address"]),
        label: text(&t["label"]),
    })
}

/// Submits a draft's exact bytes; returns the txid the node reports.
pub fn broadcast(client: &Client, hex: &str) -> Result<String, RpcError> {
    let txid = client.call("sendrawtransaction", json!([hex]))?;
    txid.as_str()
        .map(str::to_owned)
        .ok_or_else(|| RpcError::BadReply("sendrawtransaction returned no txid".into()))
}

/// The node prints amounts as JSON numbers with 9 decimals; with serde_json's
/// arbitrary_precision they keep their exact text.
fn decimal(v: &Value) -> Result<String, RpcError> {
    match v {
        Value::Number(n) => Ok(n.to_string()),
        other => Err(RpcError::BadReply(format!(
            "expected an amount, got {other}"
        ))),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn amounts_are_taken_exactly_or_refused() {
        assert_eq!(parse_amount("1.5"), Ok("1.5".into()));
        assert_eq!(parse_amount("0.000000001"), Ok("0.000000001".into()));
        assert_eq!(
            parse_amount("27226021.595212715"),
            Ok("27226021.595212715".into())
        );
        for bad in [
            "",
            "0",
            "0.000",
            ".5",
            "5.",
            "1.0000000001",
            "-1",
            "+1",
            "1e3",
            "1,5",
            " 1",
            "12345678901",
        ] {
            assert!(parse_amount(bad).is_err(), "accepted {bad:?}");
        }
    }

    #[test]
    fn activity_keeps_amounts_exact() {
        let t: Value = serde_json::from_str(
            r#"{"txid": "ab", "category": "send", "amount": -0.123456789,
                "fee": -0.000141, "confirmations": 3, "time": 1700000000,
                "address": "b3q", "label": ""}"#,
        )
        .unwrap();
        let e = parse_activity(&t).unwrap();
        assert_eq!(e.amount, "-0.123456789");
        assert_eq!(e.fee.as_deref(), Some("-0.000141"));
        assert_eq!(e.confirmations, 3);
        let t = json!({"txid": "cd", "category": "generate", "amount": 5});
        assert_eq!(parse_activity(&t).unwrap().fee, None);
        assert!(parse_activity(&json!({"amount": "5"})).is_err());
    }

    #[test]
    fn decimals_keep_their_exact_text() {
        let v: Value = serde_json::from_str("{\"a\": 0.100000001}").unwrap();
        assert_eq!(decimal(&v["a"]).unwrap(), "0.100000001");
        assert!(decimal(&json!("1")).is_err());
    }
}
