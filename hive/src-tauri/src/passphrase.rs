//! The passphrase window's requests: encrypt a wallet or change its
//! passphrase. The main window only asks for one and hears done, cancelled
//! or failed; the passphrase is typed in the passphrase window, handed to
//! the core, used for one RPC call and dropped. It is never logged, stored
//! or sent to another window.

use serde::{Deserialize, Serialize};
use serde_json::json;

use crate::rpc::{Client, RpcError};
use crate::wallet::{self, Security};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Kind {
    /// Qt's Encrypt wallet: a new passphrase, typed twice.
    Encrypt,
    /// Qt's Change passphrase: the old one, then the new one twice.
    Change,
}

/// What the passphrase window shows. The id ties a submit to this request.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Request {
    pub id: String,
    pub kind: Kind,
    pub wallet: String,
}

/// What the passphrase window sends back. Fields a kind does not use are
/// ignored.
#[derive(Deserialize)]
pub struct Entry {
    #[serde(default)]
    pub old: String,
    pub new: String,
    pub repeat: String,
}

// Entries never reach a log through Debug.
impl std::fmt::Debug for Entry {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("Entry { .. }")
    }
}

/// The request makes sense for the wallet's current protection.
pub fn check_kind(kind: Kind, security: Security) -> Result<(), String> {
    match (kind, security) {
        (_, Security::WatchOnly) => Err("A watch-only wallet has no keys to protect.".into()),
        (Kind::Encrypt, Security::Unencrypted) => Ok(()),
        (Kind::Encrypt, _) => {
            Err("This wallet is already encrypted; change its passphrase instead.".into())
        }
        (Kind::Change, Security::Unencrypted) => Err("This wallet is not encrypted yet.".into()),
        (Kind::Change, _) => Ok(()),
    }
}

/// Checks the typed passphrases before the node sees them.
pub fn check_entry(kind: Kind, entry: &Entry) -> Result<(), String> {
    if kind == Kind::Change && entry.old.is_empty() {
        return Err("Enter the current passphrase.".into());
    }
    if entry.new.is_empty() {
        return Err("Enter a new passphrase.".into());
    }
    if entry.new != entry.repeat {
        return Err("The new passphrases do not match.".into());
    }
    // The node would cut a passphrase at a zero byte (Bitcoin Core #27067).
    if entry.new.contains('\0') || entry.old.contains('\0') {
        return Err("A passphrase cannot contain a zero byte.".into());
    }
    if kind == Kind::Change && entry.old == entry.new {
        return Err("The new passphrase is the same as the current one.".into());
    }
    Ok(())
}

/// Runs the request against the node. The wallet's protection is checked
/// again here, since it may have changed while the window was open.
pub fn apply(client: &Client, request: &Request, entry: &Entry) -> Result<(), String> {
    check_entry(request.kind, entry)?;
    let security = wallet::security(client, &request.wallet).map_err(|e| e.to_string())?;
    check_kind(request.kind, security)?;
    let result = match request.kind {
        Kind::Encrypt => client.call_wallet(&request.wallet, "encryptwallet", json!([entry.new])),
        Kind::Change => client.call_wallet(
            &request.wallet,
            "walletpassphrasechange",
            json!([entry.old, entry.new]),
        ),
    };
    result.map(|_| ()).map_err(|e| match e {
        // RPC_WALLET_PASSPHRASE_INCORRECT
        RpcError::Rpc { code: -14, .. } => "The current passphrase is not correct.".into(),
        other => other.to_string(),
    })
}

/// Qt's Lock wallet: forgets the decryption key now.
pub fn lock(client: &Client, wallet: &str) -> Result<(), String> {
    client
        .call_wallet(wallet, "walletlock", json!([]))
        .map(|_| ())
        .map_err(|e| e.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn entry(old: &str, new: &str, repeat: &str) -> Entry {
        Entry {
            old: old.into(),
            new: new.into(),
            repeat: repeat.into(),
        }
    }

    #[test]
    fn requests_fit_the_wallet() {
        assert!(check_kind(Kind::Encrypt, Security::Unencrypted).is_ok());
        assert!(check_kind(Kind::Encrypt, Security::Locked).is_err());
        assert!(check_kind(Kind::Change, Security::Unencrypted).is_err());
        assert!(check_kind(Kind::Change, Security::Unlocked).is_ok());
        assert!(check_kind(Kind::Encrypt, Security::WatchOnly).is_err());
    }

    #[test]
    fn typed_passphrases_are_checked() {
        assert!(check_entry(Kind::Encrypt, &entry("", "horse staple", "horse staple")).is_ok());
        assert!(check_entry(Kind::Encrypt, &entry("", "", "")).is_err());
        assert!(check_entry(Kind::Encrypt, &entry("", "a", "b")).is_err());
        assert!(check_entry(Kind::Encrypt, &entry("", "a\0b", "a\0b")).is_err());
        assert!(check_entry(Kind::Change, &entry("", "n", "n")).is_err());
        assert!(check_entry(Kind::Change, &entry("o", "o", "o")).is_err());
        assert!(check_entry(Kind::Change, &entry("o", "n", "n")).is_ok());
    }

    #[test]
    fn entries_do_not_print() {
        let e = entry("old secret", "new secret", "new secret");
        assert_eq!(format!("{e:?}"), "Entry { .. }");
    }
}
