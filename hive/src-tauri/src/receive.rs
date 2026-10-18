//! Receive: new addresses, Qt's b3coin: payment links and their QR codes, and
//! the wallet's receiving addresses.
//!
//! Addresses come from the node's default type. On mainnet the node allows
//! only legacy B3 addresses, because witness outputs are not safe under the
//! legacy consensus; Qt's Receive tab offers only that type there too.

use qrcode::{Color, EcLevel, QrCode};
use serde::Serialize;
use serde_json::{json, Value};

use crate::rpc::{Client, RpcError};
use crate::wallet::{decimal, parse_amount};

/// A QR code as rows of modules, '1' for dark, read left to right and top to
/// bottom; the page draws it.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Qr {
    pub width: usize,
    pub modules: String,
}

/// What the Receive page shows for one address.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Request {
    pub address: String,
    pub label: String,
    pub amount: Option<String>,
    pub message: String,
    pub uri: String,
    /// None when the link is too long for a QR code, as Qt reports.
    pub qr: Option<Qr>,
}

/// Qt's limit for a payment link in a QR code (MAX_URI_LENGTH).
const MAX_URI_LENGTH: usize = 255;

/// Percent-encodes everything except RFC 3986 unreserved characters, as
/// QUrl::toPercentEncoding does for Qt's links.
fn percent(text: &str) -> String {
    let mut out = String::new();
    for b in text.bytes() {
        if b.is_ascii_alphanumeric() || matches!(b, b'-' | b'.' | b'_' | b'~') {
            out.push(b as char);
        } else {
            out.push_str(&format!("%{b:02X}"));
        }
    }
    out
}

/// Qt's formatBitcoinURI: b3coin:ADDRESS?amount=..&label=..&message=..,
/// with bech32 addresses in capitals so the QR code stays small.
pub fn uri(
    address: &str,
    bech32_hrp: &str,
    amount: Option<&str>,
    label: &str,
    message: &str,
) -> String {
    let bech32 = address
        .to_ascii_lowercase()
        .starts_with(&format!("{bech32_hrp}1"));
    let mut uri = format!(
        "b3coin:{}",
        if bech32 {
            address.to_ascii_uppercase()
        } else {
            address.to_owned()
        }
    );
    let mut params = Vec::new();
    if let Some(a) = amount {
        params.push(format!("amount={a}"));
    }
    if !label.is_empty() {
        params.push(format!("label={}", percent(label)));
    }
    if !message.is_empty() {
        params.push(format!("message={}", percent(message)));
    }
    if !params.is_empty() {
        uri.push('?');
        uri.push_str(&params.join("&"));
    }
    uri
}

pub fn qr(text: &str) -> Option<Qr> {
    if text.len() > MAX_URI_LENGTH {
        return None;
    }
    // Qt uses QR_ECLEVEL_L.
    let code = QrCode::with_error_correction_level(text.as_bytes(), EcLevel::L).ok()?;
    let modules = code
        .to_colors()
        .into_iter()
        .map(|c| if c == Color::Dark { '1' } else { '0' })
        .collect();
    Some(Qr {
        width: code.width(),
        modules,
    })
}

/// The chain's bech32 prefix, for the link's capitals.
fn hrp(chain: &str) -> &'static str {
    match chain {
        "main" => "b3",
        "regtest" => "bcrt",
        _ => "tb",
    }
}

fn text(input: &str, what: &str) -> Result<String, String> {
    let t = input.trim();
    if t.chars().count() > 200 {
        return Err(format!("Keep the {what} under 200 characters."));
    }
    if t.chars().any(char::is_control) {
        return Err(format!(
            "The {what} cannot contain line breaks or control characters."
        ));
    }
    Ok(t.to_owned())
}

pub fn request(
    address: String,
    chain: &str,
    label: String,
    amount: Option<String>,
    message: String,
) -> Request {
    let uri = uri(&address, hrp(chain), amount.as_deref(), &label, &message);
    let qr = qr(&uri);
    Request {
        address,
        label,
        amount,
        message,
        uri,
        qr,
    }
}

/// Qt's "Create new receiving address": a new address under `label`. The
/// amount and message only go into the link.
pub fn new_request(
    client: &Client,
    wallet: &str,
    chain: &str,
    label: &str,
    amount: &str,
    message: &str,
) -> Result<Request, String> {
    let label = text(label, "label")?;
    let message = text(message, "message")?;
    let amount = match amount.trim() {
        "" => None,
        a => Some(parse_amount(a)?),
    };
    let address = client
        .call_wallet(wallet, "getnewaddress", json!([label]))
        .map_err(|e| e.to_string())?;
    let address = address
        .as_str()
        .ok_or("The node returned no address.")?
        .to_owned();
    Ok(request(address, chain, label, amount, message))
}

/// One of the wallet's receiving addresses, as Qt's Receiving addresses
/// window lists them, with what it has received.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Receiving {
    pub address: String,
    pub label: String,
    /// Exact decimal text, unconfirmed payments included.
    pub received: String,
    pub transactions: usize,
}

pub fn addresses(client: &Client, wallet: &str) -> Result<Vec<Receiving>, RpcError> {
    // minconf 0, include addresses that never received anything.
    let list = client.call_wallet(wallet, "listreceivedbyaddress", json!([0, true]))?;
    parse_addresses(&list)
}

pub(crate) fn parse_addresses(list: &Value) -> Result<Vec<Receiving>, RpcError> {
    let mut out = Vec::new();
    for item in list.as_array().into_iter().flatten() {
        out.push(Receiving {
            address: item["address"].as_str().unwrap_or_default().to_owned(),
            label: item["label"].as_str().unwrap_or_default().to_owned(),
            received: decimal(&item["amount"])?,
            transactions: item["txids"].as_array().map_or(0, Vec::len),
        });
    }
    Ok(out)
}

/// The link and QR code for one of the wallet's own receiving addresses.
pub fn show(client: &Client, wallet: &str, chain: &str, address: &str) -> Result<Request, String> {
    let list = addresses(client, wallet).map_err(|e| e.to_string())?;
    let found = list
        .into_iter()
        .find(|r| r.address == address)
        .ok_or("That is not one of this wallet's receiving addresses.")?;
    Ok(request(
        found.address,
        chain,
        found.label,
        None,
        String::new(),
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn links_match_qt() {
        assert_eq!(uri("Sabc", "b3", None, "", ""), "b3coin:Sabc");
        assert_eq!(
            uri("Sabc", "b3", Some("1.5"), "Rent & food", "for März"),
            "b3coin:Sabc?amount=1.5&label=Rent%20%26%20food&message=for%20M%C3%A4rz"
        );
        // Bech32 in capitals, only for this chain's prefix.
        assert_eq!(
            uri("bcrt1qxyz", "bcrt", None, "a", ""),
            "b3coin:BCRT1QXYZ?label=a"
        );
        assert_eq!(uri("bcrt1qxyz", "b3", None, "", ""), "b3coin:bcrt1qxyz");
    }

    #[test]
    fn qr_codes_are_square_and_capped() {
        let q = qr("b3coin:BCRT1QXYZ").unwrap();
        assert_eq!(q.modules.len(), q.width * q.width);
        assert!(q.modules.bytes().all(|b| b == b'0' || b == b'1'));
        assert!(qr(&"x".repeat(MAX_URI_LENGTH + 1)).is_none());
    }

    #[test]
    fn labels_and_messages_are_one_line() {
        assert_eq!(text("  rent ", "label").unwrap(), "rent");
        assert!(text("a\nb", "label").is_err());
        assert!(text(&"x".repeat(201), "message").is_err());
    }

    #[test]
    fn receiving_addresses_carry_exact_amounts() {
        let list = serde_json::from_str::<Value>(
            r#"[{"address":"bcrt1qa","amount":0.100000001,"confirmations":0,"label":"rent","txids":["t1","t2"]},
                {"address":"bcrt1qb","amount":0,"confirmations":0,"label":"","txids":[]}]"#,
        )
        .unwrap();
        let r = parse_addresses(&list).unwrap();
        assert_eq!(r[0].received, "0.100000001");
        assert_eq!(r[0].transactions, 2);
        assert_eq!(r[1].label, "");
    }
}
