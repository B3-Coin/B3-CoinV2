//! Wallet calls: the wallet list and picking the pages' wallet, its balance,
//! activity, regtest funding, and building a signed send for the approval
//! window.
//!
//! The pages use the wallet picked in B3 Hive while it stays loaded, else the
//! first loaded wallet. Mainnet never creates a wallet unasked; regtest
//! creates its own test wallet when none is loaded.

use std::sync::Mutex;

use serde::{Deserialize, Serialize};
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

/// The file a restore or import reads, as a path the node can open.
fn source_file(path: &std::path::Path) -> Result<&str, String> {
    if !path.is_file() {
        return Err(format!("{} is not a file.", path.display()));
    }
    path.to_str()
        .ok_or_else(|| format!("{} cannot be passed to the node.", path.display()))
}

/// Qt's Restore wallet: loads a backup file as a new wallet `name` that
/// loads at every start. Restoring rescans, so `client` has no time limit.
pub fn restore(client: &Client, name: &str, backup: &std::path::Path) -> Result<String, String> {
    let name = check_new_name(name)?;
    let file = source_file(backup)?;
    let list = wallets(client).map_err(|e| e.to_string())?;
    if list.loaded.contains(&name) || list.available.contains(&name) {
        return Err(
            "A wallet with that name is already in the wallets folder. Choose a new name.".into(),
        );
    }
    let _one_at_a_time = LOADING.lock().unwrap_or_else(|e| e.into_inner());
    client
        .call("restorewallet", json!([name, file, true]))
        .map_err(|e| e.to_string())?;
    Ok(name)
}

/// What a legacy import recovered.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Imported {
    pub wallet: String,
    pub keys_imported: u64,
    pub keys_already_present: u64,
}

/// Recovers a legacy B3 wallet from the text file its dumpwallet RPC wrote,
/// into a new descriptor wallet `name` (importlegacywalletdump). The node
/// checks the whole file before it changes anything, then rescans the whole
/// chain, so `client` has no time limit. A wallet a failed import left blank
/// is used again, so the import can be retried under the same name.
pub fn import_legacy(
    client: &Client,
    name: &str,
    dump: &std::path::Path,
) -> Result<Imported, String> {
    let name = check_new_name(name)?;
    let file = source_file(dump)?;
    let list = wallets(client).map_err(|e| e.to_string())?;
    {
        let _one_at_a_time = LOADING.lock().unwrap_or_else(|e| e.into_inner());
        if list.loaded.contains(&name) || list.available.contains(&name) {
            if !list.loaded.contains(&name) {
                client
                    .call("loadwallet", json!([name, true]))
                    .map_err(|e| e.to_string())?;
            }
            if !is_blank(client, &name)? {
                return Err(
                    "A wallet with that name is already in the wallets folder. Choose a new name."
                        .into(),
                );
            }
        } else {
            client
                .call(
                    "createwallet",
                    json!([name, false, true, null, false, true, true]),
                )
                .map_err(|e| e.to_string())?;
        }
    }
    match client.call_wallet(&name, "importlegacywalletdump", json!([file])) {
        Ok(done) => Ok(Imported {
            keys_imported: done["keys_imported"].as_u64().unwrap_or(0),
            keys_already_present: done["keys_already_present"].as_u64().unwrap_or(0),
            wallet: name,
        }),
        Err(e) if is_blank(client, &name).unwrap_or(false) => Err(format!(
            "{e} Nothing was imported; wallet {name} is still empty. Import again under the same name to retry."
        )),
        Err(e) => Err(format!(
            "{e} The keys are in wallet {name}; run rescanblockchain for it in the Console to find its coins."
        )),
    }
}

/// A wallet that has no keys yet, as createwallet's blank option makes it.
fn is_blank(client: &Client, wallet: &str) -> Result<bool, String> {
    let info = client
        .call_wallet(wallet, "getwalletinfo", json!([]))
        .map_err(|e| e.to_string())?;
    Ok(info["blank"] == Value::Bool(true))
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

/// One recipient, as the Send page fills it in.
#[derive(Debug, Clone, Default, Deserialize)]
pub struct Recipient {
    pub address: String,
    pub amount: String,
    /// Saved in the address book once the payment is sent; may be empty.
    #[serde(default)]
    pub label: String,
    /// The network fee comes out of this amount instead of on top.
    #[serde(default)]
    pub subtract_fee: bool,
}

/// A payment as the Send page asks for it.
#[derive(Debug, Clone, Default, Deserialize)]
pub struct Payment {
    pub recipients: Vec<Recipient>,
    /// B3 per 1000 virtual bytes; empty lets the wallet choose.
    #[serde(default)]
    pub fee_rate: String,
    /// Spend exactly these coins (coin control); empty lets the wallet choose.
    #[serde(default)]
    pub coins: Vec<Outpoint>,
}

pub const MAX_RECIPIENTS: usize = 50;
const MAX_LABEL: usize = 200;

/// The walletcreatefundedpsbt arguments for a payment, plus the labels to
/// save once it is sent. Amounts go over the wire as strings, so the node
/// parses the exact digits.
pub(crate) fn funding_args(payment: &Payment) -> Result<(Value, Vec<(String, String)>), String> {
    let count = payment.recipients.len();
    if count == 0 {
        return Err("Add at least one recipient.".into());
    }
    if count > MAX_RECIPIENTS {
        return Err(format!(
            "A payment can have at most {MAX_RECIPIENTS} recipients."
        ));
    }
    let mut outputs = Vec::new();
    let mut seen = std::collections::HashSet::new();
    let mut subtract = Vec::new();
    let mut labels = Vec::new();
    for (i, r) in payment.recipients.iter().enumerate() {
        let which = |e: String| {
            if count > 1 {
                format!("Recipient {}: {e}", i + 1)
            } else {
                e
            }
        };
        let address = r.address.trim();
        if address.is_empty() {
            return Err(which("Enter a destination address.".into()));
        }
        let amount = parse_amount(r.amount.trim()).map_err(which)?;
        if !seen.insert(address) {
            return Err(which(
                "This address is already paid in this payment.".into(),
            ));
        }
        let label = check_label(&r.label).map_err(which)?;
        if !label.is_empty() {
            labels.push((address.to_owned(), label.to_owned()));
        }
        if r.subtract_fee {
            subtract.push(i);
        }
        let mut output = serde_json::Map::new();
        output.insert(address.to_owned(), Value::String(amount));
        outputs.push(Value::Object(output));
    }

    let mut options = json!({"lockUnspents": true});
    if !subtract.is_empty() {
        options["subtractFeeFromOutputs"] = json!(subtract);
    }
    let fee_rate = payment.fee_rate.trim();
    if !fee_rate.is_empty() {
        let rate = parse_amount(fee_rate)
            .map_err(|_| "Enter the fee rate in B3 per kvB, like 0.0001, or leave it empty.")?;
        options["feeRate"] = Value::String(rate);
    }
    let mut inputs = Vec::new();
    for coin in &payment.coins {
        let hex = coin.txid.len() == 64
            && coin
                .txid
                .bytes()
                .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b));
        if !hex {
            return Err("A chosen coin is not valid. Choose the coins again.".into());
        }
        if inputs.contains(&coin) {
            return Err("A coin was chosen twice. Choose the coins again.".into());
        }
        inputs.push(coin);
    }
    if !inputs.is_empty() {
        // Only the chosen coins, never others added to make up the amount.
        options["add_inputs"] = Value::Bool(false);
    }
    Ok((json!([inputs, outputs, 0, options]), labels))
}

/// The unsigned transaction inside a PSBT (BIP 174 global key 0x00), as
/// hex. decodepsbt would also decode every input's previous transaction,
/// which for hundreds of legacy inputs is tens of megabytes of JSON.
pub(crate) fn psbt_unsigned_tx(psbt: &str) -> Result<String, String> {
    use base64::Engine;
    let bytes = base64::engine::general_purpose::STANDARD
        .decode(psbt)
        .map_err(|_| PSBT_UNREADABLE)?;
    let mut map = Cursor(bytes.strip_prefix(b"psbt\xff").ok_or(PSBT_UNREADABLE)?);
    loop {
        let key_len = map.compact_size()?;
        if key_len == 0 {
            return Err(PSBT_UNREADABLE.into()); // the end of the global map
        }
        let key = map.take(key_len)?;
        let value_len = map.compact_size()?;
        let value = map.take(value_len)?;
        if key == [0x00] {
            return Ok(value.iter().map(|b| format!("{b:02x}")).collect());
        }
    }
}

const PSBT_UNREADABLE: &str = "The funded transaction could not be read.";

/// Reads a byte string from the front.
struct Cursor<'a>(&'a [u8]);

impl<'a> Cursor<'a> {
    fn take(&mut self, len: usize) -> Result<&'a [u8], String> {
        if len > self.0.len() {
            return Err(PSBT_UNREADABLE.into());
        }
        let (part, rest) = self.0.split_at(len);
        self.0 = rest;
        Ok(part)
    }

    /// A Bitcoin compact-size integer.
    fn compact_size(&mut self) -> Result<usize, String> {
        let width = match self.take(1)?[0] {
            0xfd => 2,
            0xfe => 4,
            0xff => 8,
            n => return Ok(n as usize),
        };
        let value = self
            .take(width)?
            .iter()
            .rev()
            .fold(0u64, |v, b| (v << 8) | u64::from(*b));
        usize::try_from(value).map_err(|_| PSBT_UNREADABLE.into())
    }
}

/// An exact decimal amount (at most 9 places) in base units.
pub(crate) fn base_units(amount: &str) -> Option<u128> {
    let (whole, frac) = amount.split_once('.').unwrap_or((amount, ""));
    if frac.len() > 9 || !(whole.bytes().chain(frac.bytes())).all(|b| b.is_ascii_digit()) {
        return None;
    }
    let whole: u128 = if whole.is_empty() {
        0
    } else {
        whole.parse().ok()?
    };
    let frac: u128 = format!("{frac:0<9}").parse().ok()?;
    whole.checked_mul(1_000_000_000)?.checked_add(frac)
}

/// Base units as an exact 9-place amount.
pub(crate) fn units_amount(units: u128) -> String {
    format!("{}.{:09}", units / 1_000_000_000, units % 1_000_000_000)
}

/// `a < b` for exact decimal amounts; amounts it cannot read compare as not less.
pub(crate) fn amount_less(a: &str, b: &str) -> bool {
    matches!((base_units(a), base_units(b)), (Some(a), Some(b)) if a < b)
}

/// A label as the address book keeps it: trimmed, one line, at most
/// MAX_LABEL characters. Empty is allowed.
pub(crate) fn check_label(label: &str) -> Result<&str, String> {
    let label = label.trim();
    if label.chars().count() > MAX_LABEL || label.chars().any(char::is_control) {
        return Err(format!(
            "Labels are at most {MAX_LABEL} characters, on one line."
        ));
    }
    Ok(label)
}

/// Funds a single-recipient send; see [`prepare_payment`].
pub fn prepare_send(
    client: &Client,
    wallet: &str,
    chain: &str,
    address: &str,
    amount: &str,
) -> Result<(SendSummary, Funded), String> {
    let payment = Payment {
        recipients: vec![Recipient {
            address: address.to_owned(),
            amount: amount.to_owned(),
            ..Recipient::default()
        }],
        ..Payment::default()
    };
    prepare_payment(client, wallet, chain, &payment)
}

/// Funds a payment and decodes it, without signing. The coins it spends stay
/// reserved (lockUnspents) until the draft is decided; nothing is broadcast.
/// The caller stores the result as a draft.
pub fn prepare_payment(
    client: &Client,
    wallet: &str,
    chain: &str,
    payment: &Payment,
) -> Result<(SendSummary, Funded), String> {
    let (args, labels) = funding_args(payment)?;
    if !payment.coins.is_empty() {
        // The page offers only listunspent's coins, but it is not trusted:
        // a stake chosen here would be spent, which unstakes it.
        let spendable = coins(client, wallet).map_err(|e| e.to_string())?;
        check_chosen(&spendable, &payment.coins, payment.recipients.len())?;
    }
    fund(client, wallet, chain, args, labels)
}

/// Funds walletcreatefundedpsbt `args` and decodes the result, without
/// signing; outputs to this wallet's own addresses are marked as such.
pub(crate) fn fund(
    client: &Client,
    wallet: &str,
    chain: &str,
    args: Value,
    labels: Vec<(String, String)>,
) -> Result<(SendSummary, Funded), String> {
    let funded = client
        .call_wallet(wallet, "walletcreatefundedpsbt", args)
        // The node's wording comes from Bitcoin Core.
        .map_err(|e| {
            e.to_string()
                .replace("Invalid Bitcoin address", "Not a valid B3 address")
        })?;
    let change_pos = funded["changepos"].as_i64().unwrap_or(-1);
    let fee = decimal(&funded["fee"]).map_err(|e| e.to_string())?;
    let psbt = funded["psbt"]
        .as_str()
        .ok_or("funding returned no PSBT")?
        .to_owned();

    let decoded = client
        .call("decoderawtransaction", json!([psbt_unsigned_tx(&psbt)?]))
        .map_err(|e| e.to_string())?;
    let (inputs, mut outputs) = parse_tx(&decoded, change_pos)?;
    for line in outputs.iter_mut().filter(|l| !l.change) {
        line.mine = client
            .call_wallet(wallet, "getaddressinfo", json!([line.address]))
            .is_ok_and(|info| info["ismine"] == Value::Bool(true));
    }
    Ok((
        SendSummary {
            chain: chain.to_owned(),
            wallet: wallet.to_owned(),
            outputs,
            fee,
            inputs: inputs.len(),
        },
        Funded {
            psbt,
            inputs,
            labels,
        },
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
            mine: false,
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

/// Saves a sent payment's labels in the address book. A label that cannot be
/// saved does not undo the payment, so failures are ignored.
pub fn save_labels(client: &Client, wallet: &str, labels: &[(String, String)]) {
    for (address, label) in labels {
        let _ = client.call_wallet(wallet, "setlabel", json!([address, label]));
    }
}

/// One coin the wallet can spend, for coin control.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Coin {
    pub txid: String,
    pub vout: u64,
    pub address: String,
    pub label: String,
    pub amount: String,
    pub confirmations: u64,
    /// The virtual bytes spending it adds to a transaction (an estimate).
    pub vbytes: u64,
}

/// The coins for coin control, and how many virtual bytes of them fit in one
/// transaction paying one address plus change.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct CoinList {
    pub coins: Vec<Coin>,
    pub input_budget: u64,
}

/// B3's standard-transaction limit: MAX_STANDARD_TX_WEIGHT / 4. The node
/// refuses to relay anything larger.
pub const MAX_TX_VBYTES: u64 = 100_000;
/// Version, lock time and the input and output counts.
const TX_OVERHEAD_VBYTES: u64 = 11;
/// The largest common output (pay to taproot); others are smaller.
const OUTPUT_VBYTES: u64 = 43;

/// The size estimate for one payment: its chosen coins, one output per
/// recipient and one for change.
fn tx_vbytes(inputs: u64, recipients: usize) -> u64 {
    TX_OVERHEAD_VBYTES + inputs + OUTPUT_VBYTES * (recipients as u64 + 1)
}

/// The virtual bytes one input adds when it spends this script, signed as
/// the wallet signs (72-byte signatures). Scripts it cannot tell get a
/// generous figure, so a selection errs on the small side.
pub(crate) fn input_vbytes(script: &str, desc: &str) -> u64 {
    let is = |prefix: &str, len: usize, suffix: &str| {
        script.len() == len && script.starts_with(prefix) && script.ends_with(suffix)
    };
    if is("76a914", 50, "88ac") {
        // Pay to public key hash: the key is in the input, 33 bytes or 65.
        let key = desc
            .split_once("pkh(")
            .map(|(_, k)| k.rsplit(']').next().unwrap_or(k))
            .and_then(|k| k.split(')').next());
        return match key {
            Some(k) if k.len() == 66 => 148,
            _ => 180,
        };
    }
    if is("21", 70, "ac") {
        return 114; // pay to a compressed public key, as legacy stake rewards are
    }
    if is("41", 134, "ac") {
        return 146; // pay to an uncompressed public key
    }
    if is("0014", 44, "") {
        return 68;
    }
    if is("5120", 68, "") {
        return 58;
    }
    if is("a914", 46, "87") && desc.starts_with("sh(wpkh(") {
        return 91;
    }
    300
}

/// Refuses chosen coins the wallet cannot spend (gone, reserved, a stake or
/// an asset) and selections too large for one standard transaction.
pub(crate) fn check_chosen(
    spendable: &[Coin],
    chosen: &[Outpoint],
    recipients: usize,
) -> Result<(), String> {
    let sizes: std::collections::HashMap<(&str, u64), u64> = spendable
        .iter()
        .map(|c| ((c.txid.as_str(), c.vout), c.vbytes))
        .collect();
    let mut inputs = 0;
    for coin in chosen {
        inputs += sizes.get(&(coin.txid.as_str(), coin.vout)).ok_or(
            "A chosen coin can no longer be spent from this wallet. Choose the coins again.",
        )?;
    }
    let size = tx_vbytes(inputs, recipients);
    if size > MAX_TX_VBYTES {
        return Err(format!(
            "{} coins are too many for one transaction: about {} kB, and the limit is {} kB. Choose fewer and combine the rest in another payment.",
            chosen.len(),
            size.div_ceil(1000),
            MAX_TX_VBYTES / 1000
        ));
    }
    Ok(())
}

/// The coins for coin control; see [`coins`].
pub fn coin_list(client: &Client, wallet: &str) -> Result<CoinList, RpcError> {
    Ok(CoinList {
        coins: coins(client, wallet)?,
        input_budget: MAX_TX_VBYTES - tx_vbytes(0, 1),
    })
}

/// The wallet's spendable coins, largest first. Coins held back for a
/// payment waiting in the approval window are not listed.
pub fn coins(client: &Client, wallet: &str) -> Result<Vec<Coin>, RpcError> {
    let unspent = client.call_wallet(wallet, "listunspent", json!([0]))?;
    let mut coins = parse_coins(&unspent)?;
    coins.sort_by(|a, b| {
        let key = |c: &Coin| c.amount.parse::<f64>().unwrap_or(0.0);
        key(b).total_cmp(&key(a))
    });
    Ok(coins)
}

pub(crate) fn parse_coins(v: &Value) -> Result<Vec<Coin>, RpcError> {
    let list = v
        .as_array()
        .ok_or_else(|| RpcError::BadReply("listunspent returned no list".into()))?;
    let mut coins = Vec::new();
    for u in list {
        if u["spendable"].as_bool() == Some(false) {
            continue;
        }
        let txid = u["txid"].as_str().unwrap_or_default().to_owned();
        let vout = u["vout"].as_u64().unwrap_or_default();
        coins.push(Coin {
            txid,
            vout,
            address: u["address"].as_str().unwrap_or_default().to_owned(),
            label: u["label"].as_str().unwrap_or_default().to_owned(),
            amount: decimal(&u["amount"])?,
            confirmations: u["confirmations"].as_u64().unwrap_or_default(),
            vbytes: input_vbytes(
                u["scriptPubKey"].as_str().unwrap_or_default(),
                u["desc"].as_str().unwrap_or_default(),
            ),
        });
    }
    Ok(coins)
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

    fn to(address: &str, amount: &str) -> Recipient {
        Recipient {
            address: address.into(),
            amount: amount.into(),
            ..Recipient::default()
        }
    }

    #[test]
    fn amounts_compare_exactly() {
        assert!(amount_less("0.000000999", "0.000001"));
        assert!(!amount_less("0.000001000", "0.000001"));
        assert!(amount_less("9.99", "10"));
        assert!(!amount_less("10", "9.999999999"));
        assert!(!amount_less("x", "1"));
    }

    #[test]
    fn the_unsigned_transaction_is_read_from_a_psbt() {
        use base64::Engine;
        let b64 = |bytes: &[u8]| base64::engine::general_purpose::STANDARD.encode(bytes);
        let tx: Vec<u8> = (0..300u16).map(|n| n as u8).collect();
        let mut psbt = b"psbt\xff".to_vec();
        // Another global key first, then the transaction with a 3-byte length.
        psbt.extend([0x02, 0xfc, 0x01, 0x01, 0xaa]);
        psbt.extend([0x01, 0x00, 0xfd, 0x2c, 0x01]);
        psbt.extend(&tx);
        psbt.push(0x00);
        let hex: String = tx.iter().map(|b| format!("{b:02x}")).collect();
        assert_eq!(psbt_unsigned_tx(&b64(&psbt)).unwrap(), hex);

        assert!(psbt_unsigned_tx("not base64!").is_err());
        assert!(psbt_unsigned_tx(&b64(b"psbx\xff\x00")).is_err());
        // No transaction before the map ends, or a length past the end.
        assert!(psbt_unsigned_tx(&b64(b"psbt\xff\x00")).is_err());
        assert!(psbt_unsigned_tx(&b64(b"psbt\xff\x01\x00\x05\x01")).is_err());
    }

    #[test]
    fn inputs_are_sized_by_their_script() {
        let h160 = "ab".repeat(20);
        let p2pkh = format!("76a914{h160}88ac");
        let key = format!("02{}", "cd".repeat(32));
        let wide = format!("04{}", "cd".repeat(64));
        assert_eq!(
            input_vbytes(&p2pkh, &format!("pkh([d34db33f/44h/1h/0h/0/1]{key})#x")),
            148
        );
        assert_eq!(input_vbytes(&p2pkh, &format!("pkh({key})#x")), 148);
        // An uncompressed key, or none to tell by, is the larger input.
        assert_eq!(input_vbytes(&p2pkh, &format!("pkh({wide})#x")), 180);
        assert_eq!(input_vbytes(&p2pkh, ""), 180);
        assert_eq!(input_vbytes(&format!("21{key}ac"), ""), 114);
        assert_eq!(input_vbytes(&format!("41{wide}ac"), ""), 146);
        assert_eq!(input_vbytes(&format!("0014{h160}"), ""), 68);
        assert_eq!(input_vbytes(&format!("5120{}", "ef".repeat(32)), ""), 58);
        assert_eq!(
            input_vbytes(&format!("a914{h160}87"), "sh(wpkh(02aa))#x"),
            91
        );
        assert_eq!(
            input_vbytes(&format!("a914{h160}87"), "sh(multi(1,02aa))#x"),
            300
        );
        assert_eq!(input_vbytes("6a00", ""), 300);
    }

    #[test]
    fn chosen_coins_must_be_spendable_and_fit_one_transaction() {
        let coin = |n: u64| Coin {
            txid: format!("{n:064x}"),
            vout: 0,
            address: String::new(),
            label: String::new(),
            amount: "0.1".into(),
            confirmations: 1,
            vbytes: 148,
        };
        let spendable: Vec<Coin> = (0..800).map(coin).collect();
        let pick = |range: std::ops::Range<u64>| -> Vec<Outpoint> {
            range
                .map(|n| Outpoint {
                    txid: format!("{n:064x}"),
                    vout: 0,
                })
                .collect()
        };
        assert!(check_chosen(&spendable, &pick(0..3), 1).is_ok());
        // A coin listunspent does not offer, such as a stake, is refused.
        let mut stake = pick(0..1);
        stake[0].vout = 1;
        assert!(check_chosen(&spendable, &stake, 1)
            .unwrap_err()
            .contains("can no longer be spent"));
        // 675 P2PKH inputs fit; 676 do not.
        let budget = MAX_TX_VBYTES - tx_vbytes(0, 1);
        assert_eq!(budget / 148, 675);
        assert!(check_chosen(&spendable, &pick(0..675), 1).is_ok());
        let err = check_chosen(&spendable, &pick(0..676), 1).unwrap_err();
        assert!(err.starts_with("676 coins are too many"), "{err}");
    }

    #[test]
    fn payments_become_exact_funding_arguments() {
        let mut first = to(" a1 ", "1.5");
        first.label = " Rent ".into();
        let mut second = to("a2", "0.000000001");
        second.subtract_fee = true;
        let payment = Payment {
            recipients: vec![first, second],
            fee_rate: "0.0002".into(),
            coins: vec![Outpoint {
                txid: "ab".repeat(32),
                vout: 3,
            }],
        };
        let (args, labels) = funding_args(&payment).unwrap();
        assert_eq!(
            args,
            json!([
                [{"txid": "ab".repeat(32), "vout": 3}],
                [{"a1": "1.5"}, {"a2": "0.000000001"}],
                0,
                {
                    "lockUnspents": true,
                    "subtractFeeFromOutputs": [1],
                    "feeRate": "0.0002",
                    "add_inputs": false
                }
            ])
        );
        assert_eq!(labels, vec![("a1".to_owned(), "Rent".to_owned())]);

        // Without choices the wallet picks coins and fee itself.
        let (args, labels) = funding_args(&Payment {
            recipients: vec![to("a1", "2")],
            ..Payment::default()
        })
        .unwrap();
        assert_eq!(args, json!([[], [{"a1": "2"}], 0, {"lockUnspents": true}]));
        assert!(labels.is_empty());
    }

    #[test]
    fn bad_payments_are_refused_before_the_node() {
        let refused = |p: Payment| funding_args(&p).unwrap_err();
        let of = |recipients| Payment {
            recipients,
            ..Payment::default()
        };
        assert_eq!(refused(of(vec![])), "Add at least one recipient.");
        assert_eq!(
            refused(of(vec![to("a", "1"), to("a", "2")])),
            "Recipient 2: This address is already paid in this payment."
        );
        assert_eq!(
            refused(of(vec![to(" ", "1")])),
            "Enter a destination address."
        );
        assert!(
            refused(of(vec![to("a", "1"), to("b", "1.0000000001")])).starts_with("Recipient 2: ")
        );
        assert!(refused(of(vec![to("a", "1"); MAX_RECIPIENTS + 1])).contains("at most"));
        let mut labelled = to("a", "1");
        labelled.label = "two
lines"
            .into();
        assert!(refused(of(vec![labelled])).contains("Labels"));
        let mut fee = of(vec![to("a", "1")]);
        fee.fee_rate = "-1".into();
        assert!(refused(fee).contains("fee rate"));
        let coin = |txid: &str| Outpoint {
            txid: txid.into(),
            vout: 0,
        };
        let mut bad = of(vec![to("a", "1")]);
        bad.coins = vec![coin("AB")];
        assert!(refused(bad).contains("not valid"));
        let mut twice = of(vec![to("a", "1")]);
        twice.coins = vec![coin(&"a".repeat(64)), coin(&"a".repeat(64))];
        assert!(refused(twice).contains("twice"));
    }

    #[test]
    fn a_signed_transaction_must_match_its_draft() {
        let line = |address: &str, amount: &str, change| OutputLine {
            address: address.into(),
            amount: amount.into(),
            change,
            mine: false,
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
            labels: vec![],
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
