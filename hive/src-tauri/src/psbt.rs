//! PSBTs made elsewhere, as Qt's PSBT operations dialog handles them: read
//! one, sign it with this wallet, or broadcast it once it is complete.
//! Signing and broadcasting are approved in the approval window; the page
//! only reads and saves.
//!
//! A PSBT is never decoded with decodepsbt: that also decodes every input's
//! previous transaction, tens of megabytes for hundreds of legacy inputs.
//! analyzepsbt gives the fee and the signing progress, and the transaction
//! itself is read from the PSBT.

use base64::Engine;
use serde::Serialize;
use serde_json::{json, Value};

use crate::drafts::{Outpoint, OutputLine};
use crate::rpc::Client;
use crate::wallet::{self, decimal, parse_tx, psbt_unsigned_tx};

/// The largest PSBT accepted, as base64 text.
const MAX_PSBT_CHARS: usize = 32 * 1024 * 1024;

/// What the page and the approval window show of a PSBT.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Summary {
    pub chain: String,
    pub wallet: String,
    pub outputs: Vec<OutputLine>,
    /// None when the PSBT does not carry every input's amount.
    pub fee: Option<String>,
    pub inputs: usize,
    /// Inputs already signed and finalized.
    pub finalized: usize,
    /// Every input is signed: it can be broadcast.
    pub complete: bool,
}

/// A PSBT read by the core: its text and the coins it spends, which are
/// checked again before anything is signed or sent.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Loaded {
    pub psbt: String,
    pub inputs: Vec<Outpoint>,
}

/// A PSBT as the page sees it: the summary and the normalized text.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct View {
    pub psbt: String,
    pub summary: Summary,
}

/// Base64 text without the line breaks and spaces copies often carry.
pub fn normalize(text: &str) -> Result<String, String> {
    let psbt: String = text.split_whitespace().collect();
    if psbt.is_empty() {
        return Err("Paste a PSBT, or choose a PSBT file.".into());
    }
    if psbt.len() > MAX_PSBT_CHARS {
        return Err("This PSBT is too large.".into());
    }
    Ok(psbt)
}

/// Reads a PSBT for this wallet.
pub fn inspect(
    client: &Client,
    wallet: &str,
    chain: &str,
    text: &str,
) -> Result<(Summary, Loaded), String> {
    let psbt = normalize(text)?;
    let unsigned = psbt_unsigned_tx(&psbt).map_err(|_| "This is not a valid PSBT.")?;
    let analysis = client
        .call("analyzepsbt", json!([psbt]))
        .map_err(|_| "This is not a valid PSBT.")?;
    let decoded = client
        .call("decoderawtransaction", json!([unsigned]))
        .map_err(|e| e.to_string())?;
    let (inputs, mut outputs) = parse_tx(&decoded, -1)?;
    for line in outputs.iter_mut() {
        line.mine = client
            .call_wallet(wallet, "getaddressinfo", json!([line.address]))
            .is_ok_and(|info| info["ismine"] == Value::Bool(true));
    }
    let (fee, finalized, complete) = progress(&analysis);
    Ok((
        Summary {
            chain: chain.to_owned(),
            wallet: wallet.to_owned(),
            outputs,
            fee,
            inputs: inputs.len(),
            finalized,
            complete,
        },
        Loaded { psbt, inputs },
    ))
}

/// The fee, the finalized inputs and whether it is complete, from analyzepsbt.
pub(crate) fn progress(analysis: &Value) -> (Option<String>, usize, bool) {
    let fee = decimal(&analysis["fee"]).ok();
    let finalized = analysis["inputs"].as_array().map_or(0, |inputs| {
        inputs
            .iter()
            .filter(|i| i["is_final"] == Value::Bool(true))
            .count()
    });
    (fee, finalized, analysis["next"] == "extractor")
}

/// How much signing an analyzepsbt result still needs: each input not yet
/// final counts one, plus the signatures and keys it is missing.
pub(crate) fn remaining(analysis: &Value) -> usize {
    let missing = |input: &Value, what: &str| input["missing"][what].as_array().map_or(0, Vec::len);
    analysis["inputs"].as_array().map_or(0, |inputs| {
        inputs
            .iter()
            .filter(|i| i["is_final"] != Value::Bool(true))
            .map(|i| 1 + missing(i, "signatures") + missing(i, "pubkeys"))
            .sum()
    })
}

/// Signs what this wallet can and returns the new PSBT and whether it is now
/// complete. Refuses when the wallet adds nothing, or when the transaction
/// it signed is not the one approved.
pub fn sign(client: &Client, wallet: &str, loaded: &Loaded) -> Result<(String, bool), String> {
    let signed = client
        .call_wallet(wallet, "walletprocesspsbt", json!([loaded.psbt, true]))
        .map_err(|e| e.to_string())?;
    let psbt = signed["psbt"]
        .as_str()
        .ok_or("signing returned no PSBT")?
        .to_owned();
    if psbt_unsigned_tx(&psbt)? != psbt_unsigned_tx(&loaded.psbt)? {
        return Err("The signed PSBT differs from the one you approved.".into());
    }
    // walletprocesspsbt also adds key details for this wallet's outputs, so
    // a changed PSBT is not proof of a signature: the signing must advance.
    let before = client
        .call("analyzepsbt", json!([loaded.psbt]))
        .map_err(|e| e.to_string())?;
    let after = client
        .call("analyzepsbt", json!([psbt]))
        .map_err(|e| e.to_string())?;
    if remaining(&after) >= remaining(&before) {
        return Err(
            "This wallet holds none of the keys this PSBT still needs, so nothing was signed."
                .into(),
        );
    }
    Ok((psbt, signed["complete"] == Value::Bool(true)))
}

/// Finalizes a complete PSBT and broadcasts it, if it still spends the
/// approved coins and pays the approved outputs. Returns the txid.
pub fn send(client: &Client, summary: &Summary, loaded: &Loaded) -> Result<String, String> {
    let finalized = client
        .call("finalizepsbt", json!([loaded.psbt]))
        .map_err(|e| e.to_string())?;
    if finalized["complete"] != Value::Bool(true) {
        return Err("This PSBT is not fully signed yet.".into());
    }
    let hex = finalized["hex"]
        .as_str()
        .ok_or("finalizing returned no transaction")?;
    let decoded = client
        .call("decoderawtransaction", json!([hex]))
        .map_err(|e| e.to_string())?;
    let (inputs, outputs) = parse_tx(&decoded, -1)?;
    let same = inputs == loaded.inputs
        && outputs.len() == summary.outputs.len()
        && outputs
            .iter()
            .zip(&summary.outputs)
            .all(|(a, b)| a.address == b.address && a.amount == b.amount);
    if !same {
        return Err(
            "The transaction differs from the one you approved, so it was not sent.".into(),
        );
    }
    wallet::broadcast(client, hex).map_err(|e| e.to_string())
}

/// The PSBT as the binary file Qt saves.
pub fn file_bytes(psbt: &str) -> Result<Vec<u8>, String> {
    base64::engine::general_purpose::STANDARD
        .decode(normalize(psbt)?)
        .map_err(|_| "This is not a valid PSBT.".into())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pasted_psbts_lose_their_line_breaks() {
        assert_eq!(normalize(" cHNi\ndP8A\r\n").unwrap(), "cHNidP8A");
        assert!(normalize("  \n").is_err());
        assert!(file_bytes("not base64!").is_err());
        assert_eq!(file_bytes("cHNidP8A").unwrap(), b"psbt\xff\x00");
    }

    #[test]
    fn progress_is_read_from_the_analysis() {
        let n = |s: &str| Value::Number(serde_json::Number::from_string_unchecked(s.into()));
        let half = json!({
            "inputs": [{"is_final": true}, {"is_final": false, "next": "signer"}],
            "fee": n("0.000114600"),
            "next": "signer"
        });
        assert_eq!(progress(&half), (Some("0.000114600".into()), 1, false));
        let done = json!({"inputs": [{"is_final": true}], "next": "extractor"});
        assert_eq!(progress(&done), (None, 1, true));
        assert_eq!(remaining(&done), 0);
        let two = json!({"inputs": [{"is_final": false, "missing": {"signatures": ["a", "b"]}}]});
        assert_eq!(remaining(&half), 1);
        assert_eq!(remaining(&two), 3);
    }
}
