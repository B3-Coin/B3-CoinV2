//! The address book: addresses this wallet pays, with their labels, like
//! Qt's "Sending addresses". The node can add and relabel entries but has no
//! RPC to remove one, so neither can B3 Hive.

use serde::Serialize;
use serde_json::json;

use crate::rpc::{Client, RpcError};
use crate::wallet::check_label;

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct Contact {
    pub address: String,
    pub label: String,
}

/// The wallet's sending addresses, by label then address.
pub fn contacts(client: &Client, wallet: &str) -> Result<Vec<Contact>, RpcError> {
    let labels = client.call_wallet(wallet, "listlabels", json!(["send"]))?;
    let labels: Vec<&str> = labels
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(|label| label.as_str())
        .collect();
    // One request for every label's addresses, not one per label.
    let params: Vec<_> = labels.iter().map(|label| json!([label])).collect();
    let replies = client.batch_wallet(wallet, "getaddressesbylabel", &params)?;
    let mut contacts = Vec::new();
    for (label, addresses) in labels.iter().zip(replies) {
        contacts.extend(parse_label(label, &addresses?));
    }
    contacts.sort_by(|a, b| {
        (a.label.to_lowercase(), &a.address).cmp(&(b.label.to_lowercase(), &b.address))
    });
    Ok(contacts)
}

/// getaddressesbylabel's reply, keeping only addresses the wallet pays.
pub(crate) fn parse_label(label: &str, reply: &serde_json::Value) -> Vec<Contact> {
    reply
        .as_object()
        .into_iter()
        .flatten()
        .filter(|(_, info)| info["purpose"].as_str() == Some("send"))
        .map(|(address, _)| Contact {
            address: address.clone(),
            label: label.to_owned(),
        })
        .collect()
}

/// Adds an address to the book, or renames it.
pub fn save(client: &Client, wallet: &str, address: &str, label: &str) -> Result<Contact, String> {
    let address = address.trim();
    if address.is_empty() {
        return Err("Enter the address to save.".into());
    }
    let label = check_label(label)?;
    let info = client
        .call_wallet(wallet, "getaddressinfo", json!([address]))
        .map_err(|e| match e {
            // RPC_INVALID_ADDRESS_OR_KEY
            RpcError::Rpc { code: -5, .. } => "Not a valid B3 address.".to_owned(),
            other => other.to_string(),
        })?;
    if info["ismine"].as_bool() == Some(true) {
        return Err(
            "This is one of this wallet's own addresses, so it is not added to the address book."
                .into(),
        );
    }
    client
        .call_wallet(wallet, "setlabel", json!([address, label]))
        .map_err(|e| e.to_string())?;
    Ok(Contact {
        address: address.to_owned(),
        label: label.to_owned(),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_addresses_the_wallet_pays_are_contacts() {
        let reply = json!({
            "addr1": {"purpose": "send"},
            "addr2": {"purpose": "receive"},
        });
        assert_eq!(
            parse_label("Shop", &reply),
            vec![Contact {
                address: "addr1".into(),
                label: "Shop".into()
            }]
        );
        assert!(parse_label("x", &json!(null)).is_empty());
    }
}
