//! The Stake page: Modern PoS staking with this wallet's validator, as Qt's
//! validator controller does it. Creating a stake and binding the finality
//! key send transactions, so they go through the approval window. Starting
//! the staking loop needs the wallet unlocked only for that call: the loop
//! keeps its own copy of the validator and finality keys, so the wallet is
//! locked again straight after and staking goes on.

use serde::Serialize;
use serde_json::{json, Value};

use crate::drafts::{Funded, Outpoint, SendSummary};
use crate::rpc::{Client, RpcError};
use crate::wallet::{self, decimal, parse_amount, Security};

/// How long starting the loop unlocks a locked wallet for. The calls take
/// well under a second; the wallet is locked again as soon as they finish.
const START_UNLOCK_SECONDS: u64 = 60;

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Stake {
    pub txid: String,
    pub vout: u64,
    pub amount: String,
    /// UNCONFIRMED | PENDING | ACTIVE
    pub status: String,
    pub confirmations: i64,
    pub active_at_height: Option<i64>,
}

/// The node's staking loop.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Loop {
    pub running: bool,
    pub state: String,
    pub last_error: Option<String>,
    /// The loop stakes with this wallet's validator key.
    pub ours: bool,
    pub finality_signing: bool,
    pub blocks_produced: u64,
}

/// This wallet's finality key binding and set membership.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Finality {
    pub bound: bool,
    pub revoked: bool,
    /// The bound key is this wallet's. The node can tell only while the
    /// wallet is unlocked, so a locked wallet reports None.
    pub key_is_ours: Option<bool>,
    /// In the validator set in force (block-eligible).
    pub member: bool,
    /// Its weight in that set, in whole B3.
    pub weight: u64,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Status {
    pub validator_key: Option<String>,
    pub tip_height: i64,
    /// legacy | corridor | modern_pos: the rules for the next block.
    pub phase: String,
    pub min_stake: Option<String>,
    pub activation_depth: i64,
    pub active_weight: u64,
    pub total_active_weight: u64,
    pub stakes: Vec<Stake>,
    pub active: String,
    pub pending: String,
    pub unconfirmed: String,
    #[serde(rename = "loop")]
    pub staking: Loop,
    /// Absent until the wallet has a validator key.
    pub finality: Option<Finality>,
}

pub fn status(client: &Client, wallet: &str) -> Result<Status, RpcError> {
    let info = client.call_wallet(wallet, "getstakinginfo", json!([]))?;
    let finality = if info["validator_key"].is_string() {
        Some(client.call_wallet(wallet, "getfinalityinfo", json!([]))?)
    } else {
        None
    };
    parse_status(&info, finality.as_ref())
}

pub(crate) fn parse_status(info: &Value, finality: Option<&Value>) -> Result<Status, RpcError> {
    let text = |v: &Value| v.as_str().map(str::to_owned);
    let validator_key = text(&info["validator_key"]);
    let mut stakes = Vec::new();
    for s in info["stakes"].as_array().into_iter().flatten() {
        stakes.push(Stake {
            txid: text(&s["txid"]).unwrap_or_default(),
            vout: s["vout"].as_u64().unwrap_or_default(),
            amount: decimal(&s["amount"])?,
            status: text(&s["status"]).unwrap_or_default(),
            confirmations: s["confirmations"].as_i64().unwrap_or_default(),
            active_at_height: s["active_at_height"].as_i64(),
        });
    }
    let l = &info["staking"];
    let staking = Loop {
        running: l["running"].as_bool() == Some(true),
        state: text(&l["state"]).unwrap_or_default(),
        last_error: text(&l["last_error"]),
        ours: validator_key.is_some() && text(&l["validator_key"]) == validator_key,
        finality_signing: l["finality_signing"].as_bool() == Some(true),
        blocks_produced: l["blocks_produced"].as_u64().unwrap_or_default(),
    };
    let finality = finality.map(|f| Finality {
        bound: f["binding"]["bound"].as_bool() == Some(true),
        revoked: f["binding"]["revoked"].as_bool() == Some(true),
        key_is_ours: f["binding"]["key_is_ours"].as_bool(),
        member: f["validator_set"]["member"].as_bool() == Some(true),
        weight: f["validator_set"]["weight"].as_u64().unwrap_or_default(),
    });
    Ok(Status {
        validator_key,
        tip_height: info["tip_height"].as_i64().unwrap_or_default(),
        phase: text(&info["next_block_phase"]).unwrap_or_default(),
        min_stake: match &info["min_stake_amount"] {
            Value::Null => None,
            v => Some(decimal(v)?),
        },
        activation_depth: info["activation_depth"].as_i64().unwrap_or_default(),
        active_weight: info["active_weight"].as_u64().unwrap_or_default(),
        total_active_weight: info["total_active_weight"].as_u64().unwrap_or_default(),
        stakes,
        active: decimal(&info["active"])?,
        pending: decimal(&info["pending"])?,
        unconfirmed: decimal(&info["unconfirmed"])?,
        staking,
        finality,
    })
}

/// Checks a stake amount before the approval window shows it.
pub fn check_stake(client: &Client, wallet: &str, amount: &str) -> Result<String, String> {
    let amount = parse_amount(amount.trim())?;
    let info = status(client, wallet).map_err(|e| e.to_string())?;
    if info.phase == "legacy" {
        return Err("Stakes can be created only after the B3 transition.".into());
    }
    let Some(min) = info.min_stake else {
        return Err("This network has no minimum stake, so stakes cannot be created.".into());
    };
    if wallet::amount_less(&amount, &min) {
        return Err(format!("The minimum stake is {min} B3."));
    }
    Ok(amount)
}

/// Locks `amount` as a stake for this wallet's validator. Returns its txid.
pub fn create(client: &Client, wallet: &str, amount: &str) -> Result<String, String> {
    let r = client
        .call_wallet(wallet, "createstake", json!([amount]))
        .map_err(|e| e.to_string())?;
    txid(&r)
}

/// Binds (or rotates) this wallet's BLS finality key. Returns its txid.
pub fn bind(client: &Client, wallet: &str) -> Result<String, String> {
    let r = client
        .call_wallet(wallet, "bindfinalitykey", json!([]))
        .map_err(|e| e.to_string())?;
    txid(&r)
}

fn txid(reply: &Value) -> Result<String, String> {
    reply["txid"]
        .as_str()
        .map(str::to_owned)
        .ok_or_else(|| "The node returned no transaction id.".into())
}

/// Qt's FinalityStartError: staking may start only when this wallet holds
/// the finality signing key it will need.
pub(crate) fn start_error(finality: &Value) -> Option<String> {
    let set = &finality["validator_set"];
    if set["member"].as_bool() == Some(true) {
        let current = finality["signing"]["snapshot_keys"]
            .as_array()
            .into_iter()
            .flatten()
            .find(|s| s["current"].as_bool() == Some(true));
        return match current {
            Some(s) if s["key_available"].as_bool() == Some(true) => None,
            Some(_) => Some("This wallet does not hold the signing key required by the current validator set. Use the wallet containing the original key and keep its finality_signer folder.".into()),
            None => Some("The current validator set's signing key could not be checked. Refresh and try again.".into()),
        };
    }
    let binding = &finality["binding"];
    if binding["bound"].as_bool() != Some(true) || binding["revoked"].as_bool() == Some(true) {
        return Some(
            "Bind this wallet's finality key, and wait for it to confirm, before starting staking."
                .into(),
        );
    }
    if binding["key_is_ours"].as_bool() != Some(true) {
        return Some("This wallet does not hold the finality key bound to its validator.".into());
    }
    None
}

/// Starts the node's staking loop. The wallet must already be unlocked or
/// unencrypted. Like Qt, a loop without its finality signer is stopped again.
fn start_unlocked(client: &Client, wallet: &str) -> Result<(), String> {
    let finality = client
        .call_wallet(wallet, "getfinalityinfo", json!([]))
        .map_err(|e| e.to_string())?;
    if let Some(error) = start_error(&finality) {
        return Err(error);
    }
    let r = client
        .call_wallet(wallet, "startstaking", json!([]))
        .map_err(|e| e.to_string())?;
    if r["finality_signing"].as_bool() != Some(true) {
        return Err(match stop_loop(client, wallet) {
            Ok(()) => "Staking was stopped because the finality signer was not armed.".to_owned(),
            Err(e) => format!(
                "The finality signer was not armed, and staking could not be confirmed stopped ({e}). Stop staking before trying again."
            ),
        });
    }
    Ok(())
}

/// Starts staking. A locked wallet is unlocked with `passphrase` for these
/// calls only and locked again; if it cannot be locked, staking is stopped,
/// as Qt does, so it never stays unlocked behind the user's back.
pub fn start(client: &Client, wallet: &str, passphrase: Option<&str>) -> Result<(), String> {
    let info = status(client, wallet).map_err(|e| e.to_string())?;
    if info.staking.running {
        return Err(if info.staking.ours {
            "Staking is already running with this wallet.".into()
        } else {
            "This node is already staking with another wallet's validator.".into()
        });
    }
    let security = wallet::security(client, wallet).map_err(|e| e.to_string())?;
    let encrypted = match security {
        Security::WatchOnly => {
            return Err("A watch-only wallet has no validator keys to stake with.".into())
        }
        Security::Unencrypted => false,
        Security::Locked | Security::Unlocked => true,
    };
    if encrypted {
        let passphrase = passphrase.unwrap_or("");
        if passphrase.is_empty() {
            return Err("Enter the wallet passphrase to start staking.".into());
        }
        client
            .call_wallet(
                wallet,
                "walletpassphrase",
                json!([passphrase, START_UNLOCK_SECONDS]),
            )
            .map_err(|e| match e {
                // RPC_WALLET_PASSPHRASE_INCORRECT
                RpcError::Rpc { code: -14, .. } => "The passphrase is not correct.".to_owned(),
                other => other.to_string(),
            })?;
    }
    let started = start_unlocked(client, wallet);
    if !encrypted {
        return started;
    }
    let locked = client.call_wallet(wallet, "walletlock", json!([])).is_ok()
        && wallet::security(client, wallet).ok() == Some(Security::Locked);
    match (started, locked) {
        (Ok(()), true) => Ok(()),
        (Ok(()), false) => {
            Err(match stop_loop(client, wallet) {
                Ok(()) => "The wallet could not be locked after starting staking, so staking was stopped. Lock the wallet and try again.".to_owned(),
                Err(e) => format!(
                    "The wallet could not be locked after starting staking, and staking could not be confirmed stopped ({e}). Lock the wallet and stop staking now."
                ),
            })
        }
        (Err(e), true) => Err(e),
        (Err(e), false) => Err(format!(
            "{e} The wallet is still unlocked; lock it before trying again."
        )),
    }
}

fn stop_loop(client: &Client, wallet: &str) -> Result<(), String> {
    client
        .call_wallet(wallet, "stopstaking", json!([]))
        .map(|_| ())
        .map_err(|e| e.to_string())
}

/// Stops the node's staking loop, if it stakes with this wallet's validator.
pub fn stop(client: &Client, wallet: &str) -> Result<(), String> {
    // Ownership is known from the staking info alone; finality details are
    // not needed, so a failing finality read cannot block a stop.
    let info = client
        .call_wallet(wallet, "getstakinginfo", json!([]))
        .map_err(|e| e.to_string())?;
    let validator_key = info["validator_key"].as_str();
    let running = info["staking"]["running"].as_bool() == Some(true);
    let ours =
        validator_key.is_some() && info["staking"]["validator_key"].as_str() == validator_key;
    if running && !ours {
        return Err(
            "Another wallet's validator is staking; switch to that wallet to stop it.".into(),
        );
    }
    stop_loop(client, wallet)
}

/// The label of the address a release pays, as the node's releasestake uses.
const RELEASE_LABEL: &str = "b3-stake-release";

/// Checks the stakes chosen for release and returns their principal: each
/// must be one of this wallet's confirmed stakes, listed once.
pub(crate) fn release_principal(stakes: &[Stake], chosen: &[Outpoint]) -> Result<String, String> {
    if chosen.is_empty() {
        return Err("Choose a stake to release.".into());
    }
    let mut total = 0u128;
    for (i, coin) in chosen.iter().enumerate() {
        if chosen[..i].contains(coin) {
            return Err("A stake was chosen twice.".into());
        }
        let stake = stakes
            .iter()
            .find(|s| s.txid == coin.txid && s.vout == coin.vout)
            .ok_or("That stake is no longer in this wallet. Refresh the page.")?;
        if stake.status == "UNCONFIRMED" {
            return Err("A stake can be released once it confirms.".into());
        }
        total += wallet::base_units(&stake.amount).ok_or("A stake amount could not be read.")?;
    }
    Ok(wallet::units_amount(total))
}

/// Funds a release (unstake) without signing: exactly the chosen STAKE
/// outputs, paid to one new address of this wallet with the fee taken from
/// the principal. It is the transaction the node's releasestake and
/// b3_docker's unstake build. Once it confirms the stake stops counting;
/// weight already in an epoch snapshot can count until that is replaced.
pub fn prepare_release(
    client: &Client,
    wallet: &str,
    chain: &str,
    chosen: &[Outpoint],
) -> Result<(SendSummary, Funded), String> {
    let info = status(client, wallet).map_err(|e| e.to_string())?;
    let principal = release_principal(&info.stakes, chosen)?;
    let address = client
        .call_wallet(wallet, "getnewaddress", json!([RELEASE_LABEL, "legacy"]))
        .map_err(|e| e.to_string())?;
    let address = address.as_str().ok_or("The node returned no address.")?;
    let mut output = serde_json::Map::new();
    output.insert(address.to_owned(), Value::String(principal));
    let args = json!([
        chosen,
        [output],
        0,
        {"add_inputs": false, "subtractFeeFromOutputs": [0], "lockUnspents": true}
    ]);
    let (summary, funded) = wallet::fund(client, wallet, chain, args, Vec::new())?;
    let exact = funded.inputs.len() == chosen.len()
        && chosen.iter().all(|c| funded.inputs.contains(c))
        && summary.outputs.len() == 1
        && summary.outputs[0].mine
        && summary.outputs[0].address == address;
    if !exact {
        wallet::release(client, wallet, &funded.inputs);
        return Err("The node built a different release, so it was not offered.".into());
    }
    Ok((summary, funded))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_confirmed_stakes_of_this_wallet_are_released() {
        let stake = |n: u64, amount: &str, status: &str| Stake {
            txid: format!("{n:064x}"),
            vout: 0,
            amount: amount.into(),
            status: status.into(),
            confirmations: 1,
            active_at_height: None,
        };
        let pick = |n: u64| Outpoint {
            txid: format!("{n:064x}"),
            vout: 0,
        };
        let stakes = [
            stake(1, "10.000000000", "ACTIVE"),
            stake(2, "0.000000001", "PENDING"),
            stake(3, "5", "UNCONFIRMED"),
        ];
        assert_eq!(
            release_principal(&stakes, &[pick(1), pick(2)]).unwrap(),
            "10.000000001"
        );
        assert!(release_principal(&stakes, &[]).is_err());
        assert!(release_principal(&stakes, &[pick(1), pick(1)])
            .unwrap_err()
            .contains("twice"));
        assert!(release_principal(&stakes, &[pick(3)])
            .unwrap_err()
            .contains("once it confirms"));
        assert!(release_principal(&stakes, &[pick(4)])
            .unwrap_err()
            .contains("no longer in this wallet"));
    }

    #[test]
    fn staking_starts_only_with_its_finality_key() {
        let unbound = json!({"validator_set": {"member": false}, "binding": {"bound": false}});
        assert!(start_error(&unbound).unwrap().contains("Bind"));
        let revoked = json!({"validator_set": {}, "binding": {"bound": true, "revoked": true}});
        assert!(start_error(&revoked).unwrap().contains("Bind"));
        let foreign =
            json!({"validator_set": {}, "binding": {"bound": true, "key_is_ours": false}});
        assert!(start_error(&foreign).unwrap().contains("does not hold"));
        let ready = json!({"validator_set": {}, "binding": {"bound": true, "key_is_ours": true}});
        assert_eq!(start_error(&ready), None);

        // A set member needs the current snapshot's key, whatever its binding.
        let member = |available: bool| {
            json!({
                "validator_set": {"member": true},
                "binding": {"bound": false},
                "signing": {"snapshot_keys": [
                    {"current": false, "key_available": true},
                    {"current": true, "key_available": available}
                ]}
            })
        };
        assert_eq!(start_error(&member(true)), None);
        assert!(start_error(&member(false)).unwrap().contains("signing key"));
        let unknown = json!({"validator_set": {"member": true}, "signing": {}});
        assert!(start_error(&unknown)
            .unwrap()
            .contains("could not be checked"));
    }

    #[test]
    fn staking_info_is_read_exactly() {
        let info = json!({
            "validator_key": "vk",
            "tip_height": 140,
            "next_block_phase": "modern_pos",
            "min_stake_amount": serde_json::Number::from_string_unchecked("0.000001000".into()),
            "activation_depth": 20,
            "active_weight": 10,
            "total_active_weight": 10,
            "staking": {"running": true, "state": "staking", "validator_key": "vk",
                        "finality_signing": true, "blocks_produced": 3},
            "stakes": [{"txid": "t", "vout": 1,
                        "amount": serde_json::Number::from_string_unchecked("10.000000000".into()),
                        "status": "ACTIVE", "confirmations": 25, "active_at_height": 136}],
            "active": serde_json::Number::from_string_unchecked("10.000000000".into()),
            "pending": serde_json::Number::from_string_unchecked("0.000000000".into()),
            "unconfirmed": serde_json::Number::from_string_unchecked("0.000000000".into()),
        });
        let s = parse_status(&info, None).unwrap();
        assert_eq!(s.min_stake.as_deref(), Some("0.000001000"));
        assert_eq!(s.stakes[0].amount, "10.000000000");
        assert!(s.staking.running && s.staking.ours);
        assert_eq!(s.finality, None);

        let mut other = info.clone();
        other["staking"]["validator_key"] = json!("someone-else");
        assert!(!parse_status(&other, None).unwrap().staking.ours);
    }
}
