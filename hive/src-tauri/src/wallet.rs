//! Wallet calls: the wallet list and picking the pages' wallet, its balance,
//! activity, regtest funding, and building a signed send for the approval
//! window.
//!
//! The pages use the wallet picked in B3 Hive while it stays loaded, else the
//! first loaded wallet. Mainnet never creates a wallet unasked; regtest
//! creates its own test wallet when none is loaded.

use std::sync::Mutex;

use serde::Serialize;
use serde_json::{json, Value};

use crate::drafts::{Funded, Outpoint, OutputLine, SendSummary};
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

/// The wallet the pages use: `preferred` while it is loaded, else the first
/// loaded wallet. On regtest the test wallet comes before the others, and is
/// created when no wallet is loaded. None when mainnet has no wallet loaded.
pub fn select(
    client: &Client,
    chain: &str,
    preferred: Option<&str>,
) -> Result<Option<String>, RpcError> {
    let loaded = loaded(client)?;
    if let Some(name) = preferred.filter(|p| loaded.iter().any(|l| l == p)) {
        return Ok(Some(name.to_owned()));
    }
    if chain == "regtest" && (loaded.is_empty() || loaded.iter().any(|l| l == SPIKE_WALLET)) {
        ensure_wallet(client)?;
        return Ok(Some(SPIKE_WALLET.to_owned()));
    }
    Ok(loaded.into_iter().next())
}

fn strings(v: &Value) -> Vec<String> {
    v.as_array()
        .map(|a| {
            a.iter()
                .filter_map(Value::as_str)
                .map(str::to_owned)
                .collect()
        })
        .unwrap_or_default()
}

/// The wallets the node has loaded, in its order.
pub fn loaded(client: &Client) -> Result<Vec<String>, RpcError> {
    Ok(strings(&client.call("listwallets", json!([]))?))
}

/// Qt's File menu lists: the loaded wallets, and the others in the node's
/// wallets folder that Open wallet offers. The default wallet's name is "".
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Wallets {
    pub loaded: Vec<String>,
    pub available: Vec<String>,
}

pub fn wallets(client: &Client) -> Result<Wallets, RpcError> {
    let loaded = loaded(client)?;
    let dir = client.call("listwalletdir", json!([]))?;
    Ok(parse_wallets(loaded, &dir))
}

pub(crate) fn parse_wallets(loaded: Vec<String>, dir: &Value) -> Wallets {
    let mut available: Vec<String> = dir["wallets"]
        .as_array()
        .map(|a| {
            a.iter()
                .filter_map(|w| w["name"].as_str())
                .filter(|n| !loaded.iter().any(|l| l == n))
                .map(str::to_owned)
                .collect()
        })
        .unwrap_or_default();
    available.sort();
    Wallets { loaded, available }
}

/// A name for a new wallet. Kept to letters, digits, spaces, dots, dashes
/// and underscores, so it is a plain folder in the wallets folder and never
/// a path elsewhere.
pub fn check_new_name(name: &str) -> Result<String, String> {
    let name = name.trim();
    if name.is_empty() {
        return Err("Enter a name for the wallet.".into());
    }
    if name.chars().count() > 64 {
        return Err("Use at most 64 characters.".into());
    }
    if name.starts_with('.')
        || !name
            .chars()
            .all(|c| c.is_alphanumeric() || matches!(c, ' ' | '.' | '-' | '_'))
    {
        return Err(
            "Use letters, digits, spaces, dots, dashes and underscores, and do not start with a dot."
                .into(),
        );
    }
    Ok(name.to_owned())
}

/// Opens a wallet from the wallets folder and loads it again at the next
/// start, as Qt's Open wallet does.
pub fn open(client: &Client, name: &str) -> Result<(), String> {
    let list = wallets(client).map_err(|e| e.to_string())?;
    if list.loaded.iter().any(|l| l == name) {
        return Ok(());
    }
    if !list.available.iter().any(|a| a == name) {
        return Err("That wallet is not in the wallets folder.".into());
    }
    let _one_at_a_time = LOADING.lock().unwrap_or_else(|e| e.into_inner());
    client
        .call("loadwallet", json!([name, true]))
        .map(|_| ())
        .map_err(|e| e.to_string())
}

/// Unloads a wallet and stops loading it at start, as Qt's Close wallet does.
pub fn close(client: &Client, name: &str) -> Result<(), String> {
    client
        .call("unloadwallet", json!([name, false]))
        .map(|_| ())
        .map_err(|e| e.to_string())
}

/// Creates a descriptor wallet with keys that loads at every start. It is
/// not encrypted; Encrypt wallet does that afterwards, as in Qt.
pub fn create(client: &Client, name: &str) -> Result<String, String> {
    let name = check_new_name(name)?;
    if wallets(client)
        .map_err(|e| e.to_string())?
        .available
        .contains(&name)
    {
        return Err(
            "A wallet with that name is already in the wallets folder; open it instead.".into(),
        );
    }
    let _one_at_a_time = LOADING.lock().unwrap_or_else(|e| e.into_inner());
    client
        .call(
            "createwallet",
            json!([name, false, false, null, false, true, true]),
        )
        .map_err(|e| e.to_string())?;
    Ok(name)
}

/// Copies the wallet to `path`, which must not exist yet.
pub fn backup(client: &Client, wallet: &str, path: &std::path::Path) -> Result<(), String> {
    if path.exists() {
        return Err(format!("{} already exists.", path.display()));
    }
    let target = path.to_str().ok_or("The backup path is not valid text.")?;
    client
        .call_wallet(wallet, "backupwallet", json!([target]))
        .map(|_| ())
        .map_err(|e| e.to_string())
}

/// Loads the regtest test wallet, creating it on first use.
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
/// The wallet's own coins, as Qt's overview splits them.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Balances {
    /// Confirmed, or this wallet's own unconfirmed change.
    pub spendable: String,
    /// Unconfirmed payments from others.
    pub pending: String,
    /// Mined or staked, not yet mature.
    pub immature: String,
}

pub fn balances(client: &Client, wallet: &str) -> Result<Balances, RpcError> {
    let b = client.call_wallet(wallet, "getbalances", json!([]))?;
    let mine = &b["mine"];
    Ok(Balances {
        spendable: decimal(&mine["trusted"])?,
        pending: decimal(&mine["untrusted_pending"])?,
        immature: decimal(&mine["immature"])?,
    })
}

/// How the wallet's keys are protected, as Qt's dashboard shows it.
#[derive(Debug, Clone, Copy, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Security {
    WatchOnly,
    Unencrypted,
    Locked,
    Unlocked,
}

pub fn parse_security(info: &Value) -> Security {
    if info["private_keys_enabled"] == Value::Bool(false) {
        return Security::WatchOnly;
    }
    // unlocked_until is present only for passphrase-encrypted wallets.
    match info.get("unlocked_until").and_then(Value::as_i64) {
        None => Security::Unencrypted,
        Some(0) => Security::Locked,
        Some(_) => Security::Unlocked,
    }
}

pub fn security(client: &Client, wallet: &str) -> Result<Security, RpcError> {
    Ok(parse_security(&client.call_wallet(
        wallet,
        "getwalletinfo",
        json!([]),
    )?))
}

/// The era and staking lines of Qt's dashboard, from getstakinginfo.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Staking {
    pub tip_height: i64,
    /// legacy | corridor | modern_pos, or modern on chains without the
    /// legacy B3 boundary (regtest): the rules for the next block.
    pub next_block_phase: String,
    pub modern_pos_active: bool,
    pub running: bool,
    /// The node's staking loop stakes with this wallet's validator key.
    pub this_wallet: bool,
    pub state: String,
    pub last_error: Option<String>,
    pub finality_signing: bool,
    /// -1 when nothing was signed yet.
    pub last_signed_height: i64,
}

pub fn parse_staking(info: &Value) -> Staking {
    let text = |v: &Value| v.as_str().unwrap_or_default().to_owned();
    let flag = |v: &Value| v.as_bool().unwrap_or(false);
    let own_key = text(&info["validator_key"]);
    let loop_ = &info["staking"];
    let running = flag(&loop_["running"]);
    Staking {
        tip_height: info["tip_height"].as_i64().unwrap_or(-1),
        next_block_phase: text(&info["next_block_phase"]),
        modern_pos_active: flag(&info["modern_pos_active"]),
        running,
        this_wallet: running && !own_key.is_empty() && text(&loop_["validator_key"]) == own_key,
        state: text(&loop_["state"]),
        last_error: loop_["last_error"]
            .as_str()
            .filter(|e| !e.is_empty())
            .map(str::to_owned),
        finality_signing: flag(&loop_["finality_signing"]),
        last_signed_height: loop_["last_signed_height"].as_i64().unwrap_or(-1),
    }
}

pub fn staking(client: &Client, wallet: &str) -> Result<Staking, RpcError> {
    Ok(parse_staking(&client.call_wallet(
        wallet,
        "getstakinginfo",
        json!([]),
    )?))
}

/// Regtest only: mines FUND_BLOCKS blocks to a fresh wallet address.
pub fn fund_regtest(client: &Client, wallet: &str) -> Result<(), RpcError> {
    let address = client.call_wallet(wallet, "getnewaddress", json!([]))?;
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

/// Funds a send to `address` and decodes it, without signing. The coins it
/// spends stay reserved (lockUnspents) until the draft is decided; nothing
/// is broadcast. The caller stores the result as a draft.
pub fn prepare_send(
    client: &Client,
    wallet: &str,
    chain: &str,
    address: &str,
    amount: &str,
) -> Result<(SendSummary, Funded), String> {
    let amount = parse_amount(amount)?;
    let address = address.trim();
    if address.is_empty() {
        return Err("Enter a destination address.".into());
    }
    // Amount goes over the wire as a string, so the node parses the exact digits.
    let mut output = serde_json::Map::new();
    output.insert(address.to_owned(), Value::String(amount));
    let funded = client
        .call_wallet(
            wallet,
            "walletcreatefundedpsbt",
            json!([[], [output], 0, {"lockUnspents": true}]),
        )
        .map_err(|e| e.to_string())?;
    let change_pos = funded["changepos"].as_i64().unwrap_or(-1);
    let fee = decimal(&funded["fee"]).map_err(|e| e.to_string())?;
    let psbt = funded["psbt"]
        .as_str()
        .ok_or("funding returned no PSBT")?
        .to_owned();

    let decoded = client
        .call("decodepsbt", json!([psbt]))
        .map_err(|e| e.to_string())?;
    let (inputs, outputs) = parse_tx(&decoded["tx"], change_pos)?;
    Ok((
        SendSummary {
            chain: chain.to_owned(),
            wallet: wallet.to_owned(),
            outputs,
            fee,
            inputs: inputs.len(),
        },
        Funded { psbt, inputs },
    ))
}

/// The coins a decoded transaction spends and the outputs it pays; the
/// output at `change_pos` is marked as change.
pub(crate) fn parse_tx(
    tx: &Value,
    change_pos: i64,
) -> Result<(Vec<Outpoint>, Vec<OutputLine>), String> {
    let mut inputs = Vec::new();
    for vin in tx["vin"]
        .as_array()
        .ok_or("the transaction has no inputs")?
    {
        inputs.push(Outpoint {
            txid: vin["txid"]
                .as_str()
                .ok_or("an input has no txid")?
                .to_owned(),
            vout: vin["vout"].as_u64().ok_or("an input has no index")?,
        });
    }
    let mut outputs = Vec::new();
    for out in tx["vout"]
        .as_array()
        .ok_or("the transaction has no outputs")?
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
    Ok((inputs, outputs))
}

/// Signs and finalizes an approved draft. The signed transaction must spend
/// exactly the drafted coins and pay exactly the approved outputs, or it is
/// refused. Returns the transaction's hex.
pub fn sign(client: &Client, summary: &SendSummary, funded: &Funded) -> Result<String, String> {
    let signed = client
        .call_wallet(
            &summary.wallet,
            "walletprocesspsbt",
            json!([funded.psbt, true]),
        )
        .map_err(|e| e.to_string())?;
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
    let (inputs, outputs) = parse_tx(&decoded, -1)?;
    check_signed(summary, funded, &inputs, &outputs)?;
    Ok(hex)
}

pub(crate) fn check_signed(
    summary: &SendSummary,
    funded: &Funded,
    inputs: &[Outpoint],
    outputs: &[OutputLine],
) -> Result<(), String> {
    let same_outputs = outputs.len() == summary.outputs.len()
        && outputs
            .iter()
            .zip(&summary.outputs)
            .all(|(a, b)| a.address == b.address && a.amount == b.amount);
    if inputs != funded.inputs.as_slice() || !same_outputs {
        return Err(
            "The signed transaction differs from the one you approved, so it was not sent.".into(),
        );
    }
    Ok(())
}

/// Frees the coins a draft reserved, when it is rejected or fails.
pub fn release(client: &Client, wallet: &str, inputs: &[Outpoint]) {
    if inputs.is_empty() {
        return;
    }
    let _ = client.call_wallet(wallet, "lockunspent", json!([true, inputs]));
}

/// How long an approval unlocks a locked wallet for. Signing takes well under
/// a second; the wallet is locked again as soon as it is done.
const SIGNING_UNLOCK_SECONDS: u64 = 60;

/// Unlocks a locked wallet to sign one approved draft.
pub fn unlock_for_signing(client: &Client, wallet: &str, passphrase: &str) -> Result<(), String> {
    if passphrase.is_empty() {
        return Err("Enter the wallet passphrase to approve this payment.".into());
    }
    client
        .call_wallet(
            wallet,
            "walletpassphrase",
            json!([passphrase, SIGNING_UNLOCK_SECONDS]),
        )
        .map(|_| ())
        .map_err(|e| match e {
            // RPC_WALLET_PASSPHRASE_INCORRECT
            RpcError::Rpc { code: -14, .. } => "The passphrase is not correct.".into(),
            other => other.to_string(),
        })
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
pub fn activity(client: &Client, wallet: &str, count: u64) -> Result<Vec<ActivityEntry>, RpcError> {
    let count = count.clamp(1, ACTIVITY_LIMIT);
    let list = client.call_wallet(wallet, "listtransactions", json!(["*", count]))?;
    let list = list
        .as_array()
        .ok_or_else(|| RpcError::BadReply("listtransactions did not return a list".into()))?;
    // The node lists oldest first.
    list.iter().rev().map(parse_activity).collect()
}

pub(crate) fn parse_activity(t: &Value) -> Result<ActivityEntry, RpcError> {
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
pub(crate) fn decimal(v: &Value) -> Result<String, RpcError> {
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
    fn a_signed_transaction_must_match_its_draft() {
        let line = |address: &str, amount: &str, change| OutputLine {
            address: address.into(),
            amount: amount.into(),
            change,
        };
        let coin = |vout| Outpoint {
            txid: "aa".repeat(32),
            vout,
        };
        let summary = SendSummary {
            chain: "regtest".into(),
            wallet: "w".into(),
            outputs: vec![line("pay", "1.000000000", false), line("chg", "0.5", true)],
            fee: "0.00001".into(),
            inputs: 1,
        };
        let funded = Funded {
            psbt: String::new(),
            inputs: vec![coin(0)],
        };
        // Signing does not know which output is change; only address and
        // amount must match.
        let signed = [line("pay", "1.000000000", false), line("chg", "0.5", false)];
        assert!(check_signed(&summary, &funded, &[coin(0)], &signed).is_ok());
        assert!(check_signed(&summary, &funded, &[coin(1)], &signed).is_err());
        assert!(check_signed(&summary, &funded, &[coin(0), coin(1)], &signed).is_err());
        let more = [line("pay", "1.000000001", false), line("chg", "0.5", false)];
        assert!(check_signed(&summary, &funded, &[coin(0)], &more).is_err());
        let elsewhere = [
            line("thief", "1.000000000", false),
            line("chg", "0.5", false),
        ];
        assert!(check_signed(&summary, &funded, &[coin(0)], &elsewhere).is_err());
        assert!(check_signed(&summary, &funded, &[coin(0)], &signed[..1]).is_err());
    }

    #[test]
    fn new_wallet_names_stay_in_the_wallets_folder() {
        assert_eq!(check_new_name("  Savings 2026 ").unwrap(), "Savings 2026");
        assert_eq!(
            check_new_name("cold_store-v1.0").unwrap(),
            "cold_store-v1.0"
        );
        for bad in [
            "",
            "   ",
            "../evil",
            "a/b",
            r"a\b",
            ".hidden",
            "C:x",
            &"x".repeat(65),
        ] {
            assert!(check_new_name(bad).is_err(), "{bad:?}");
        }
    }

    #[test]
    fn the_folder_list_leaves_out_loaded_wallets() {
        let dir = json!({"wallets": [{"name": "zeta"}, {"name": ""}, {"name": "main"}, {"name": "alpha"}]});
        let w = parse_wallets(vec!["main".into()], &dir);
        assert_eq!(w.loaded, ["main"]);
        assert_eq!(w.available, ["", "alpha", "zeta"]);
    }

    #[test]
    fn security_follows_getwalletinfo() {
        let cases = [
            (json!({"private_keys_enabled": true}), Security::Unencrypted),
            (
                json!({"private_keys_enabled": true, "unlocked_until": 0}),
                Security::Locked,
            ),
            (
                json!({"private_keys_enabled": true, "unlocked_until": 1_900_000_000}),
                Security::Unlocked,
            ),
            (json!({"private_keys_enabled": false}), Security::WatchOnly),
        ];
        for (info, want) in cases {
            assert_eq!(parse_security(&info), want, "{info}");
        }
    }

    #[test]
    fn staking_matches_qt_reading() {
        let key = "ab".repeat(32);
        let mine = parse_staking(&json!({
            "validator_key": key, "tip_height": 900, "next_block_phase": "modern_pos",
            "modern_pos_active": true,
            "staking": {"running": true, "validator_key": key, "state": "staking",
                        "last_error": "", "finality_signing": true, "last_signed_height": 899}
        }));
        assert!(mine.running && mine.this_wallet && mine.finality_signing);
        assert_eq!(mine.last_error, None);
        assert_eq!(mine.last_signed_height, 899);
        // Another wallet's loop, and a wallet with no validator key yet.
        let other = parse_staking(&json!({
            "validator_key": key, "tip_height": 900, "next_block_phase": "legacy",
            "staking": {"running": true, "validator_key": "cd".repeat(32), "last_error": "no coins"}
        }));
        assert!(other.running && !other.this_wallet);
        assert_eq!(other.last_error.as_deref(), Some("no coins"));
        let keyless = parse_staking(&json!({"staking": {"running": true, "validator_key": ""}}));
        assert!(!keyless.this_wallet);
        assert_eq!((keyless.tip_height, keyless.last_signed_height), (-1, -1));
    }

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
