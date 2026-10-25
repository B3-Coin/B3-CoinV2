//! A read-only client for the FlowMesh V2 regtest HTTPS API
//! (contrib/flowmesh2/regtest/https_api.py): `/v1/info`, `/v1/totals`,
//! `/v1/account/<id>` and the `/v1/events` certified-state stream.
//!
//! The API serves one self-signed certificate that the operator publishes
//! with its SHA-256 pin. B3 Hive trusts exactly that certificate: the file
//! must hash to the pin, and it is the only root the TLS session accepts.
//! Only loopback addresses are accepted, since remote access is an open
//! product question (hive/README.md).
//!
//! Every value shown comes from the operator. The operator checks its own
//! certificate and proofs before answering; B3 Hive does not verify the
//! FlowMesh certificate or the account proof yet, and the page says so.
//! Nothing here submits or signs: `/v1/submit` needs a signed action frame,
//! and B3 Hive has no FlowMesh V2 signer.

use std::io::{BufRead, BufReader, Read};
use std::path::Path;
use std::sync::Arc;
use std::time::Duration;

use serde::Serialize;
use serde_json::Value;
use sha2::{Digest, Sha256};
use ureq::tls::{Certificate, PemItem, RootCerts, TlsConfig, TlsProvider};

/// The API's own bound on a reply (https_api.py `_write`).
const MAX_REPLY_BYTES: u64 = 1 << 20;
/// The API's own bound on one event (https_api.py `_events`).
const MAX_EVENT_BYTES: usize = 1 << 20;
/// A certificate file is a few hundred bytes; anything this large is not one.
const MAX_CERTIFICATE_FILE_BYTES: u64 = 64 * 1024;
/// The certified-state header is 366 bytes; its height is at offset 73.
const HEADER_BYTES: usize = 366;
const HEADER_HEIGHT_OFFSET: usize = 73;

/// Where the API is and which certificate it must present.
pub struct Endpoint {
    origin: String,
    agent: ureq::Agent,
    stream_agent: ureq::Agent,
}

/// The operator's status, as `/v1/info` reports it.
#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct Info {
    pub mode: String,
    pub seat: u64,
    pub seats: u64,
    pub quorum: u64,
    pub height: u64,
    pub head_hash: String,
    pub state_root: String,
    pub certified: bool,
    pub pending_actions: u64,
    pub mainnet_enabled: bool,
    pub assets: Vec<Asset>,
    pub markets: Vec<Market>,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct Asset {
    pub name: String,
    pub id: String,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct Market {
    pub name: String,
    pub id: String,
    pub base: String,
    pub quote: String,
    /// Atom counts are decimal text: they can exceed what a page number holds.
    pub base_atoms_per_lot: String,
    pub quote_atoms_per_tick: String,
}

/// One asset's totals at the certified head, as `/v1/totals` reports them.
#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct AssetTotals {
    pub asset: String,
    pub deposits: String,
    pub available: String,
    pub reserved: String,
    pub pending: String,
    pub fees_held: String,
    pub total_liabilities: String,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct Totals {
    pub height: u64,
    pub certified: bool,
    pub conservation: bool,
    pub assets: Vec<AssetTotals>,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct Balance {
    pub asset: String,
    pub available: String,
    pub reserved: String,
}

/// One account at the certified head, as `/v1/account/<id>` reports it.
#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct Account {
    pub account: String,
    pub found: bool,
    pub height: u64,
    pub certified: bool,
    /// The operator checked the account proof against the certified head.
    pub operator_verified: bool,
    pub balances: Vec<Balance>,
    pub next_nonce: Option<String>,
}

impl Endpoint {
    /// Checks the address and the certificate before anything is sent.
    pub fn new(url: &str, certificate: &Path, certificate_sha256: &str) -> Result<Self, String> {
        let origin = loopback_origin(url)?;
        let certificate = pinned_certificate(certificate, certificate_sha256)?;
        let tls = TlsConfig::builder()
            .provider(TlsProvider::Rustls)
            .root_certs(RootCerts::Specific(Arc::new(vec![certificate])))
            .build();
        let build = |global: Option<Duration>| -> ureq::Agent {
            ureq::Agent::config_builder()
                .https_only(true)
                .max_redirects(0)
                .proxy(None)
                .http_status_as_error(false)
                .user_agent("B3Hive-FlowMeshV2")
                .tls_config(tls.clone())
                .timeout_connect(Some(Duration::from_secs(5)))
                .timeout_recv_response(Some(Duration::from_secs(10)))
                .timeout_global(global)
                .build()
                .into()
        };
        Ok(Self {
            origin,
            agent: build(Some(Duration::from_secs(15))),
            // The stream is held open for up to 30 s by the API; the read
            // timeout below still catches a silent server.
            stream_agent: build(Some(Duration::from_secs(45))),
        })
    }

    pub fn info(&self) -> Result<Info, String> {
        parse_info(&self.get("/v1/info")?)
    }

    pub fn totals(&self) -> Result<Totals, String> {
        parse_totals(&self.get("/v1/totals")?)
    }

    pub fn account(&self, account: &str) -> Result<Account, String> {
        if !is_hash_hex(account) {
            return Err("An account is 64 lowercase hexadecimal characters.".to_owned());
        }
        parse_account(&self.get(&format!("/v1/account/{account}"))?)
    }

    /// Reads one event stream from `after` until the API closes it (it does
    /// after its stream time) or `on_event` returns false. Returns the last
    /// height seen, the cursor for the next stream.
    pub fn events(
        &self,
        after: u64,
        mut on_event: impl FnMut(u64, Totals) -> bool,
    ) -> Result<u64, String> {
        let url = format!("{}/v1/events?after-height={after}", self.origin);
        let response = self
            .stream_agent
            .get(&url)
            .call()
            .map_err(transport_error)?;
        let status = response.status().as_u16();
        if status != 200 {
            return Err(format!(
                "The FlowMesh V2 API refused the event stream (HTTP {status})."
            ));
        }
        let content_type = response
            .headers()
            .get("content-type")
            .and_then(|v| v.to_str().ok())
            .unwrap_or("");
        if content_type != "text/event-stream" {
            return Err("The FlowMesh V2 API did not answer with an event stream.".to_owned());
        }
        let mut cursor = after;
        let reader = BufReader::new(response.into_body().into_reader());
        let mut parser = EventParser::default();
        for event in EventLines::new(reader) {
            let Some(event) = parser.line(&event?)? else {
                continue;
            };
            let totals = parse_totals(&event.data)?;
            if event.id != totals.height || totals.height <= cursor {
                return Err(
                    "The FlowMesh V2 event stream went backwards or disagreed with itself."
                        .to_owned(),
                );
            }
            cursor = totals.height;
            if !on_event(cursor, totals) {
                break;
            }
        }
        Ok(cursor)
    }

    fn get(&self, path: &str) -> Result<Value, String> {
        let mut response = self
            .agent
            .get(&format!("{}{path}", self.origin))
            .header("Connection", "close")
            .call()
            .map_err(transport_error)?;
        let status = response.status().as_u16();
        let body: Value = response
            .body_mut()
            .with_config()
            .limit(MAX_REPLY_BYTES)
            .read_json()
            .map_err(|_| {
                format!("The FlowMesh V2 API sent an unreadable reply (HTTP {status}).")
            })?;
        if status != 200 {
            let detail = body
                .get("error")
                .and_then(Value::as_str)
                .unwrap_or("no reason given");
            return Err(format!(
                "The FlowMesh V2 API refused the request (HTTP {status}): {detail}."
            ));
        }
        body.get("result")
            .cloned()
            .ok_or_else(|| "The FlowMesh V2 API reply has no result.".to_owned())
    }
}

fn transport_error(e: ureq::Error) -> String {
    match e {
        ureq::Error::Tls(_) | ureq::Error::Rustls(_) => {
            "The FlowMesh V2 API did not present the pinned certificate.".to_owned()
        }
        ureq::Error::ConnectionFailed => {
            "The FlowMesh V2 API is not reachable at that address.".to_owned()
        }
        // rustls reports a refused certificate as an I/O error.
        ureq::Error::Io(e) => format!("The FlowMesh V2 API connection failed: {e}."),
        ureq::Error::Timeout(_) => "The FlowMesh V2 API did not answer in time.".to_owned(),
        other => format!("The FlowMesh V2 API request failed: {other}."),
    }
}

/// `https://127.0.0.1:<port>` or `https://[::1]:<port>`, nothing else.
pub fn loopback_origin(url: &str) -> Result<String, String> {
    let refuse = || {
        "The FlowMesh V2 API address must be https://127.0.0.1:<port> or https://[::1]:<port>: \
         B3 Hive reaches it only on this machine."
            .to_owned()
    };
    let rest = url.trim().strip_prefix("https://").ok_or_else(refuse)?;
    let rest = rest.strip_suffix('/').unwrap_or(rest);
    let (host, port) = if let Some(v6) = rest.strip_prefix("[::1]:") {
        ("[::1]", v6)
    } else if let Some(v4) = rest.strip_prefix("127.0.0.1:") {
        ("127.0.0.1", v4)
    } else {
        return Err(refuse());
    };
    if port.is_empty() || port.len() > 5 || !port.bytes().all(|b| b.is_ascii_digit()) {
        return Err(refuse());
    }
    let port: u16 = port.parse().map_err(|_| refuse())?;
    if port == 0 {
        return Err(refuse());
    }
    Ok(format!("https://{host}:{port}"))
}

/// The certificate in `path`, if its DER bytes hash to `pin`.
pub fn pinned_certificate(path: &Path, pin: &str) -> Result<Certificate<'static>, String> {
    let pin = pin.trim();
    if !is_hash_hex(pin) {
        return Err("The certificate pin is 64 lowercase hexadecimal characters.".to_owned());
    }
    let file =
        std::fs::File::open(path).map_err(|e| format!("Cannot read the certificate file: {e}."))?;
    let mut text = Vec::new();
    file.take(MAX_CERTIFICATE_FILE_BYTES + 1)
        .read_to_end(&mut text)
        .map_err(|e| format!("Cannot read the certificate file: {e}."))?;
    if text.len() as u64 > MAX_CERTIFICATE_FILE_BYTES {
        return Err("The certificate file is too large to be one certificate.".to_owned());
    }
    let mut certificates = ureq::tls::parse_pem(&text).filter_map(|item| match item {
        Ok(PemItem::Certificate(c)) => Some(c),
        _ => None,
    });
    let certificate = certificates
        .next()
        .ok_or("The certificate file holds no PEM certificate.")?;
    if certificates.next().is_some() {
        return Err("The certificate file holds more than one certificate.".to_owned());
    }
    let actual = hex(&Sha256::digest(certificate.der()));
    if !equal_text(&actual, pin) {
        return Err("The certificate file does not match the pin.".to_owned());
    }
    Ok(certificate.to_owned())
}

/// Compares every byte, so the time taken does not show where they differ.
fn equal_text(a: &str, b: &str) -> bool {
    a.len() == b.len()
        && a.bytes()
            .zip(b.bytes())
            .fold(0u8, |acc, (x, y)| acc | (x ^ y))
            == 0
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn is_hash_hex(text: &str) -> bool {
    text.len() == 64 && text.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'))
}

fn field<'a>(value: &'a Value, name: &str) -> Result<&'a Value, String> {
    value
        .get(name)
        .ok_or_else(|| format!("The FlowMesh V2 API reply has no {name}."))
}

fn text(value: &Value, name: &str) -> Result<String, String> {
    field(value, name)?
        .as_str()
        .map(str::to_owned)
        .ok_or_else(|| format!("The FlowMesh V2 API reply's {name} is not text."))
}

fn flag(value: &Value, name: &str) -> Result<bool, String> {
    field(value, name)?
        .as_bool()
        .ok_or_else(|| format!("The FlowMesh V2 API reply's {name} is not true or false."))
}

/// A non-negative integer kept as its decimal text.
fn atoms(value: &Value, name: &str) -> Result<String, String> {
    let bad = || format!("The FlowMesh V2 API reply's {name} is not a whole number.");
    let number = field(value, name)?.as_number().ok_or_else(bad)?;
    let digits = number.to_string();
    if digits.is_empty() || digits.len() > 40 || !digits.bytes().all(|b| b.is_ascii_digit()) {
        return Err(bad());
    }
    Ok(digits)
}

fn count(value: &Value, name: &str) -> Result<u64, String> {
    atoms(value, name)?
        .parse()
        .map_err(|_| format!("The FlowMesh V2 API reply's {name} is out of range."))
}

fn hash(value: &Value, name: &str) -> Result<String, String> {
    let text = text(value, name)?;
    if !is_hash_hex(&text) {
        return Err(format!(
            "The FlowMesh V2 API reply's {name} is not a 32-byte hash."
        ));
    }
    Ok(text)
}

/// The height inside the hex certified-state header every proof carries.
fn header_height(value: &Value) -> Result<u64, String> {
    let header = text(value, "header")?;
    let header = header.strip_prefix("0x").unwrap_or(&header);
    if header.len() != 2 * HEADER_BYTES {
        return Err("The FlowMesh V2 API reply's header has the wrong size.".to_owned());
    }
    let at = 2 * HEADER_HEIGHT_OFFSET;
    let mut height: u64 = 0;
    for i in (0..8).rev() {
        let byte = u8::from_str_radix(&header[at + 2 * i..at + 2 * i + 2], 16)
            .map_err(|_| "The FlowMesh V2 API reply's header is not hexadecimal.".to_owned())?;
        height = (height << 8) | u64::from(byte);
    }
    Ok(height)
}

/// A certificate is present from height 1 on; height 0 is the genesis state.
fn certified(value: &Value) -> Result<bool, String> {
    match field(value, "certificate")? {
        Value::Null => Ok(false),
        Value::String(s) if !s.is_empty() => Ok(true),
        _ => Err("The FlowMesh V2 API reply's certificate is malformed.".to_owned()),
    }
}

pub fn parse_info(value: &Value) -> Result<Info, String> {
    let mut assets = Vec::new();
    for (name, id) in field(value, "assets")?
        .as_object()
        .ok_or("The FlowMesh V2 API reply's assets are not a list.")?
    {
        let id = id
            .as_str()
            .filter(|id| is_hash_hex(id))
            .ok_or("The FlowMesh V2 API reply has a malformed asset.")?;
        assets.push(Asset {
            name: name.clone(),
            id: id.to_owned(),
        });
    }
    let mut markets = Vec::new();
    for (name, market) in field(value, "markets")?
        .as_object()
        .ok_or("The FlowMesh V2 API reply's markets are not a list.")?
    {
        markets.push(Market {
            name: name.clone(),
            id: hash(market, "id")?,
            base: hash(market, "base")?,
            quote: hash(market, "quote")?,
            base_atoms_per_lot: atoms(market, "base_atoms_per_lot")?,
            quote_atoms_per_tick: atoms(market, "quote_atoms_per_tick")?,
        });
    }
    let height = count(value, "height")?;
    let info = Info {
        mode: text(value, "mode")?,
        seat: count(value, "seat")?,
        seats: count(value, "seats")?,
        quorum: count(value, "quorum")?,
        height,
        head_hash: hash(value, "head_hash")?,
        state_root: hash(value, "state_root")?,
        certified: certified(value)?,
        pending_actions: count(value, "pending_actions")?,
        mainnet_enabled: flag(value, "mainnet_enabled")?,
        assets,
        markets,
    };
    if info.certified != (height > 0) {
        return Err(
            "The FlowMesh V2 API reply's certificate does not match its height.".to_owned(),
        );
    }
    if info.mainnet_enabled {
        return Err("This FlowMesh V2 API says it is enabled for mainnet; B3 Hive reads only the regtest API.".to_owned());
    }
    Ok(info)
}

pub fn parse_totals(value: &Value) -> Result<Totals, String> {
    let mut assets = Vec::new();
    for item in field(value, "assets")?
        .as_array()
        .ok_or("The FlowMesh V2 API reply's assets are not a list.")?
    {
        assets.push(AssetTotals {
            asset: hash(item, "asset")?,
            deposits: atoms(item, "deposits")?,
            available: atoms(item, "available")?,
            reserved: atoms(item, "reserved")?,
            pending: atoms(item, "pending")?,
            fees_held: atoms(item, "fees_held")?,
            total_liabilities: atoms(item, "total_liabilities")?,
        });
    }
    let height = header_height(value)?;
    let totals = Totals {
        height,
        certified: certified(value)?,
        conservation: flag(value, "conservation")?,
        assets,
    };
    if totals.certified != (height > 0) {
        return Err(
            "The FlowMesh V2 API reply's certificate does not match its height.".to_owned(),
        );
    }
    Ok(totals)
}

pub fn parse_account(value: &Value) -> Result<Account, String> {
    let found = flag(value, "found")?;
    let height = header_height(value)?;
    let mut account = Account {
        account: hash(value, "account")?,
        found,
        height,
        certified: certified(value)?,
        operator_verified: false,
        balances: Vec::new(),
        next_nonce: None,
    };
    if !found {
        return Ok(account);
    }
    account.operator_verified = flag(value, "verified")?;
    for item in field(value, "balances")?
        .as_array()
        .ok_or("The FlowMesh V2 API reply's balances are not a list.")?
    {
        account.balances.push(Balance {
            asset: hash(item, "asset")?,
            available: atoms(item, "available")?,
            reserved: atoms(item, "reserved")?,
        });
    }
    // Absent once every nonce is used.
    account.next_nonce = match value.get("next_nonce") {
        None => None,
        Some(_) => Some(atoms(value, "next_nonce")?),
    };
    Ok(account)
}

/// One complete server-sent event.
#[derive(Debug, PartialEq)]
pub struct Event {
    pub id: u64,
    pub data: Value,
}

/// Server-sent event lines into `certified-state` events. Comments and
/// the `retry:` field are skipped; any other event name is refused.
#[derive(Default)]
pub struct EventParser {
    id: Option<String>,
    name: Option<String>,
    data: Option<String>,
}

impl EventParser {
    pub fn line(&mut self, line: &str) -> Result<Option<Event>, String> {
        if line.is_empty() {
            let (id, name, data) = (self.id.take(), self.name.take(), self.data.take());
            let Some(data) = data else { return Ok(None) };
            if name.as_deref() != Some("certified-state") {
                return Err("The FlowMesh V2 event stream sent an unknown event.".to_owned());
            }
            let id = id
                .filter(|id| {
                    !id.is_empty() && id.len() <= 20 && id.bytes().all(|b| b.is_ascii_digit())
                })
                .and_then(|id| id.parse().ok())
                .ok_or("The FlowMesh V2 event stream sent an event without a height.")?;
            let data = serde_json::from_str(&data)
                .map_err(|_| "The FlowMesh V2 event stream sent unreadable data.".to_owned())?;
            return Ok(Some(Event { id, data }));
        }
        if line.starts_with(':') {
            return Ok(None);
        }
        let (name, value) = line.split_once(':').unwrap_or((line, ""));
        let value = value.strip_prefix(' ').unwrap_or(value).to_owned();
        match name {
            "id" => self.id = Some(value),
            "event" => self.name = Some(value),
            "data" => {
                let data = self.data.get_or_insert_with(String::new);
                if !data.is_empty() {
                    data.push('\n');
                }
                data.push_str(&value);
                if data.len() > MAX_EVENT_BYTES {
                    return Err(
                        "The FlowMesh V2 event stream sent an event over its size limit."
                            .to_owned(),
                    );
                }
            }
            "retry" => {}
            _ => return Err("The FlowMesh V2 event stream sent an unknown field.".to_owned()),
        }
        Ok(None)
    }
}

/// Lines of at most `MAX_EVENT_BYTES`, without their line ending.
struct EventLines<R> {
    reader: R,
    done: bool,
}

impl<R: BufRead> EventLines<R> {
    fn new(reader: R) -> Self {
        Self {
            reader,
            done: false,
        }
    }
}

impl<R: BufRead> Iterator for EventLines<R> {
    type Item = Result<String, String>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.done {
            return None;
        }
        let mut line = Vec::new();
        let limit = MAX_EVENT_BYTES as u64 + 64;
        match (&mut self.reader).take(limit).read_until(b'\n', &mut line) {
            Ok(0) => {
                self.done = true;
                None
            }
            Ok(_) if line.last() != Some(&b'\n') => {
                self.done = true;
                if line.len() as u64 >= limit {
                    Some(Err(
                        "The FlowMesh V2 event stream sent a line over its size limit.".to_owned(),
                    ))
                } else {
                    // The API closed the stream mid-line: drop the fragment.
                    None
                }
            }
            Ok(_) => {
                line.pop();
                if line.last() == Some(&b'\r') {
                    line.pop();
                }
                Some(String::from_utf8(line).map_err(|_| {
                    "The FlowMesh V2 event stream sent text that is not UTF-8.".to_owned()
                }))
            }
            Err(e) => {
                self.done = true;
                // A stream that ends by timeout or reset ends this read; the
                // caller reconnects from its cursor.
                if matches!(
                    e.kind(),
                    std::io::ErrorKind::TimedOut | std::io::ErrorKind::ConnectionReset
                ) {
                    None
                } else {
                    Some(Err(format!("The FlowMesh V2 event stream failed: {e}.")))
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    const H: &str = "1111111111111111111111111111111111111111111111111111111111111111";
    const A: &str = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const B: &str = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

    /// A 366-byte header whose height field holds `height`.
    fn header(height: u64) -> String {
        let mut bytes = vec![0u8; HEADER_BYTES];
        bytes[HEADER_HEIGHT_OFFSET..HEADER_HEIGHT_OFFSET + 8]
            .copy_from_slice(&height.to_le_bytes());
        hex(&bytes)
    }

    fn info_reply(height: u64) -> Value {
        json!({
            "mode": "regtest-pbft-view-change", "seat": 0, "seats": 4, "quorum": 3,
            "height": height, "head_hash": H, "state_root": H,
            "certificate": if height > 0 { json!("00ff") } else { Value::Null },
            "pending_actions": 2, "mainnet_enabled": false,
            "assets": { "Q": A, "TOKENA": B },
            "markets": { "TOKENA_Q": { "id": H, "base": B, "quote": A,
                                       "base_atoms_per_lot": 1000, "quote_atoms_per_tick": 10 } },
            "profile_name": "fm2-regtest", "signer_seq": 4
        })
    }

    fn totals_reply(height: u64) -> Value {
        json!({
            "assets": [{ "asset": A, "deposits": 5000, "effects": 0, "pending": 0, "fees_held": 7,
                         "available": 4000, "reserved": 993, "protocol_available": 7,
                         "account_liabilities": 4993, "total_liabilities": 5000 }],
            "conservation": true, "header": header(height), "roots": {},
            "certificate": if height > 0 { json!("00ff") } else { Value::Null }
        })
    }

    #[test]
    fn only_loopback_https_addresses_are_accepted() {
        assert_eq!(
            loopback_origin("https://127.0.0.1:8443").unwrap(),
            "https://127.0.0.1:8443"
        );
        assert_eq!(
            loopback_origin(" https://127.0.0.1:8443/ ").unwrap(),
            "https://127.0.0.1:8443"
        );
        assert_eq!(
            loopback_origin("https://[::1]:1").unwrap(),
            "https://[::1]:1"
        );
        for bad in [
            "http://127.0.0.1:8443",
            "https://localhost:8443",
            "https://10.0.0.2:8443",
            "https://127.0.0.1",
            "https://127.0.0.1:0",
            "https://127.0.0.1:65536",
            "https://127.0.0.1:8443/v1/info",
            "https://127.0.0.1:8443@evil.example:443",
            "https://127.0.0.1.evil.example:8443",
            "https://[::1]:84a3",
        ] {
            assert!(loopback_origin(bad).is_err(), "{bad} was accepted");
        }
    }

    #[test]
    fn info_keeps_amounts_as_text_and_refuses_mainnet() {
        let info = parse_info(&info_reply(5)).unwrap();
        assert_eq!(
            (info.seats, info.quorum, info.height, info.pending_actions),
            (4, 3, 5, 2)
        );
        assert!(info.certified);
        assert_eq!(info.assets.len(), 2);
        assert_eq!(info.markets[0].name, "TOKENA_Q");
        assert_eq!(info.markets[0].base_atoms_per_lot, "1000");
        let mut mainnet = info_reply(5);
        mainnet["mainnet_enabled"] = json!(true);
        assert!(parse_info(&mainnet).is_err());
        let mut uncertified = info_reply(5);
        uncertified["certificate"] = Value::Null;
        assert!(parse_info(&uncertified).is_err());
        assert!(!parse_info(&info_reply(0)).unwrap().certified);
        let mut negative = info_reply(5);
        negative["markets"]["TOKENA_Q"]["base_atoms_per_lot"] = json!(-1);
        assert!(parse_info(&negative).is_err());
    }

    #[test]
    fn totals_take_their_height_from_the_certified_header() {
        let totals = parse_totals(&totals_reply(258)).unwrap();
        assert_eq!(totals.height, 258);
        assert!(totals.conservation && totals.certified);
        assert_eq!(totals.assets[0].total_liabilities, "5000");
        let mut short = totals_reply(1);
        short["header"] = json!("00");
        assert!(parse_totals(&short).is_err());
    }

    #[test]
    fn accounts_report_balances_and_absent_accounts() {
        let mut reply = totals_reply(3);
        reply["account"] = json!(B);
        reply["found"] = json!(false);
        let absent = parse_account(&reply).unwrap();
        assert!(!absent.found && absent.balances.is_empty() && absent.next_nonce.is_none());
        reply["found"] = json!(true);
        reply["verified"] = json!(true);
        reply["balances"] = json!([{ "asset": A, "available": 12, "reserved": 3 }]);
        reply["next_nonce"] = json!(4);
        let found = parse_account(&reply).unwrap();
        assert!(found.operator_verified);
        assert_eq!(found.balances[0].available, "12");
        assert_eq!(found.next_nonce.as_deref(), Some("4"));
    }

    #[test]
    fn event_parser_returns_certified_states_and_refuses_others() {
        let mut parser = EventParser::default();
        let data = serde_json::to_string(&totals_reply(9)).unwrap();
        for line in [
            "retry: 200",
            "",
            ": comment",
            "id: 9",
            "event: certified-state",
        ] {
            assert_eq!(parser.line(line).unwrap(), None);
        }
        assert_eq!(parser.line(&format!("data: {data}")).unwrap(), None);
        let event = parser.line("").unwrap().unwrap();
        assert_eq!(event.id, 9);
        assert_eq!(parse_totals(&event.data).unwrap().height, 9);

        let mut other = EventParser::default();
        other.line("event: something-else").unwrap();
        other.line("data: {}").unwrap();
        assert!(other.line("").is_err());
        let mut no_id = EventParser::default();
        no_id.line("event: certified-state").unwrap();
        no_id.line("data: {}").unwrap();
        assert!(no_id.line("").is_err());
        assert!(EventParser::default().line("unknown: 1").is_err());
    }

    #[test]
    fn event_lines_split_on_newlines_and_drop_a_cut_fragment() {
        let lines: Vec<_> = EventLines::new(&b"id: 1\r\nevent: x\n\nda"[..])
            .map(Result::unwrap)
            .collect();
        assert_eq!(lines, ["id: 1", "event: x", ""]);
    }

    #[test]
    fn the_certificate_must_hash_to_the_pin() {
        let dir = std::env::temp_dir().join(format!("hive-v2api-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("api.crt");
        // Any DER bytes do: the pin is over the bytes, parsing comes later.
        let der = [0x30u8, 0x03, 0x02, 0x01, 0x01];
        let pem = format!(
            "-----BEGIN CERTIFICATE-----\n{}\n-----END CERTIFICATE-----\n",
            base64::Engine::encode(&base64::engine::general_purpose::STANDARD, der)
        );
        std::fs::write(&path, pem).unwrap();
        let pin = hex(&Sha256::digest(der));
        assert!(pinned_certificate(&path, &pin).is_ok());
        let first = if pin.starts_with('0') { "1" } else { "0" };
        let wrong = format!("{first}{}", &pin[1..]);
        assert_ne!(wrong, pin);
        assert_eq!(
            pinned_certificate(&path, &wrong).unwrap_err(),
            "The certificate file does not match the pin."
        );
        assert!(pinned_certificate(&path, &pin.to_uppercase()).is_err());
        assert!(pinned_certificate(&dir.join("missing.crt"), &pin).is_err());
        std::fs::remove_dir_all(&dir).unwrap();
    }

    /// Runs against a live API when `HIVE_V2_API_FIXTURE` names its
    /// `{url, certificate, certificate_sha256, account}` JSON file; the
    /// fixture gateway in contrib or a regtest operator provides one.
    #[test]
    fn live_api_when_a_fixture_is_named() {
        let Ok(path) = std::env::var("HIVE_V2_API_FIXTURE") else {
            eprintln!("HIVE_V2_API_FIXTURE is not set: live API check skipped");
            return;
        };
        let fixture: Value = serde_json::from_slice(&std::fs::read(path).unwrap()).unwrap();
        let s = |k: &str| fixture[k].as_str().unwrap().to_owned();
        let endpoint = Endpoint::new(
            &s("url"),
            Path::new(&s("certificate")),
            &s("certificate_sha256"),
        )
        .unwrap();
        let info = endpoint.info().unwrap();
        let totals = endpoint.totals().unwrap();
        assert!(totals.height >= info.height);
        let account = endpoint.account(&s("account")).unwrap();
        assert_eq!(account.account, s("account"));
        let mut heights = Vec::new();
        let last = endpoint
            .events(info.height, |height, _| {
                heights.push(height);
                heights.len() < 2
            })
            .unwrap();
        assert!(!heights.is_empty() && heights.windows(2).all(|w| w[0] < w[1]));
        assert_eq!(last, *heights.last().unwrap());
        eprintln!(
            "live API checks passed: mode {} height {} totals {} events {:?}",
            info.mode, info.height, totals.height, heights
        );
    }
}
