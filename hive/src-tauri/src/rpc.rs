//! JSON-RPC client for the bundled b3coind on loopback, and the per-launch
//! `-rpcauth` credentials it authenticates with.

use std::fmt;
use std::time::Duration;

use base64::Engine as _;
use hmac::{Hmac, Mac};
use serde_json::{json, Value};
use sha2::Sha256;

/// The error code b3coind returns for every call until init finishes; its
/// message is the current init step (SetRPCWarmupStatus).
pub const RPC_IN_WARMUP: i64 = -28;

const PAGE_TIMEOUT: Duration = Duration::from_secs(30);

/// A login generated at launch. b3coind only ever sees `rpcauth`, the salted
/// HMAC of the password, so the process list shows nothing reusable.
pub struct Credentials {
    user: String,
    password: String,
    rpcauth: String,
}

impl Credentials {
    pub fn generate(user: &str) -> Result<Self, getrandom::Error> {
        let password = random_hex(32)?;
        let salt = random_hex(16)?;
        let rpcauth = rpcauth_value(user, &salt, &password);
        Ok(Self {
            user: user.to_owned(),
            password,
            rpcauth,
        })
    }

    pub fn user(&self) -> &str {
        &self.user
    }

    /// The value for `-rpcauth=`: `<user>:<salt>$<hmac>`.
    pub fn rpcauth(&self) -> &str {
        &self.rpcauth
    }
}

impl fmt::Debug for Credentials {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Credentials")
            .field("user", &self.user)
            .finish_non_exhaustive()
    }
}

/// Same construction as share/rpcauth/rpcauth.py: HMAC-SHA256 keyed with the
/// hex salt over the password, hex encoded.
pub fn rpcauth_value(user: &str, salt: &str, password: &str) -> String {
    let mut mac =
        Hmac::<Sha256>::new_from_slice(salt.as_bytes()).expect("HMAC accepts any key length");
    mac.update(password.as_bytes());
    format!("{user}:{salt}${}", to_hex(&mac.finalize().into_bytes()))
}

fn random_hex(bytes: usize) -> Result<String, getrandom::Error> {
    let mut buf = vec![0u8; bytes];
    getrandom::fill(&mut buf)?;
    Ok(to_hex(&buf))
}

fn to_hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

#[derive(Debug, Clone, PartialEq)]
pub enum RpcError {
    /// No HTTP exchange happened (connection refused while the node starts, timeout).
    Transport(String),
    /// The node answered with a JSON-RPC error.
    Rpc { code: i64, message: String },
    /// The node answered with an HTTP status and no JSON-RPC error (401, 403 ...).
    Http(u16),
    /// The reply was not the JSON-RPC shape we expect.
    BadReply(String),
}

impl fmt::Display for RpcError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            RpcError::Transport(e) => write!(f, "could not reach the node: {e}"),
            RpcError::Rpc { code, message } => write!(f, "{message} (code {code})"),
            RpcError::Http(status) => write!(f, "the node refused the request (HTTP {status})"),
            RpcError::BadReply(e) => write!(f, "unexpected reply from the node: {e}"),
        }
    }
}

impl std::error::Error for RpcError {}

pub struct Client {
    url: String,
    authorization: String,
    agent: ureq::Agent,
}

impl Client {
    /// A client for the pages' login; calls give up after 30 seconds.
    pub fn new(port: u16, credentials: &Credentials) -> Self {
        Self::with_login(
            port,
            &format!("{}:{}", credentials.user, credentials.password),
            Some(PAGE_TIMEOUT),
        )
    }

    /// A client without a time limit, for console commands such as
    /// rescanblockchain that can run for a long time (as in the Qt console).
    pub fn without_timeout(port: u16, credentials: &Credentials) -> Self {
        Self::with_login(
            port,
            &format!("{}:{}", credentials.user, credentials.password),
            None,
        )
    }

    /// A client using the contents of b3coind's .cookie file (`__cookie__:<secret>`).
    /// For tests and tools that need the cookie's full access, never for pages.
    pub fn from_cookie(port: u16, cookie: &str) -> Self {
        Self::with_login(port, cookie.trim(), Some(PAGE_TIMEOUT))
    }

    fn with_login(port: u16, login: &str, timeout: Option<Duration>) -> Self {
        let token = base64::engine::general_purpose::STANDARD.encode(login);
        let agent = ureq::Agent::config_builder()
            .timeout_global(timeout)
            // b3coind reports JSON-RPC errors with HTTP 500 and a JSON body.
            .http_status_as_error(false)
            .build()
            .into();
        Self {
            url: format!("http://127.0.0.1:{port}/"),
            authorization: format!("Basic {token}"),
            agent,
        }
    }

    /// Calls a node-wide method.
    pub fn call(&self, method: &str, params: Value) -> Result<Value, RpcError> {
        self.post(&self.url, method, params)
    }

    /// Calls a method on one loaded wallet (/wallet/<name>).
    pub fn call_wallet(
        &self,
        wallet: &str,
        method: &str,
        params: Value,
    ) -> Result<Value, RpcError> {
        self.post(
            &format!("{}wallet/{}", self.url, percent_encode(wallet)),
            method,
            params,
        )
    }

    fn post(&self, url: &str, method: &str, params: Value) -> Result<Value, RpcError> {
        let request = json!({ "id": "hive", "method": method, "params": params });
        let mut response = self
            .agent
            .post(url)
            .header("Authorization", &self.authorization)
            .send_json(&request)
            .map_err(|e| RpcError::Transport(e.to_string()))?;
        let status = response.status().as_u16();
        let body: Value = match response
            .body_mut()
            .with_config()
            .limit(MAX_REPLY_BYTES)
            .read_json()
        {
            Ok(body) => body,
            Err(_) if status != 200 => return Err(RpcError::Http(status)),
            Err(e) => return Err(RpcError::BadReply(e.to_string())),
        };
        parse_reply(body)
    }
}

/// The largest reply read. A PSBT carries the whole previous transaction of
/// each legacy input, so one combining hundreds of coins runs to megabytes.
const MAX_REPLY_BYTES: u64 = 64 * 1024 * 1024;

fn percent_encode(segment: &str) -> String {
    segment
        .bytes()
        .map(|b| match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => {
                (b as char).to_string()
            }
            _ => format!("%{b:02X}"),
        })
        .collect()
}

fn parse_reply(mut body: Value) -> Result<Value, RpcError> {
    match body.get("error") {
        Some(Value::Null) | None => {}
        Some(error) => {
            let code = error.get("code").and_then(Value::as_i64);
            let message = error.get("message").and_then(Value::as_str);
            return match (code, message) {
                (Some(code), Some(message)) => Err(RpcError::Rpc {
                    code,
                    message: message.to_owned(),
                }),
                _ => Err(RpcError::BadReply(format!("malformed error: {error}"))),
            };
        }
    }
    match body.get_mut("result") {
        Some(result) => Ok(result.take()),
        None => Err(RpcError::BadReply("no result".into())),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rpcauth_matches_rpcauth_py() {
        // python3: hmac.new(b"00112233445566778899aabbccddeeff", b"hunter2", "SHA256").hexdigest()
        assert_eq!(
            rpcauth_value("hive", "00112233445566778899aabbccddeeff", "hunter2"),
            "hive:00112233445566778899aabbccddeeff$f595e32e70e0cb4698af3e4b966d9966543e02f1209cbc44b88cda990a8c308d"
        );
    }

    #[test]
    fn generated_credentials_are_fresh_and_well_formed() {
        let a = Credentials::generate("hive").unwrap();
        let b = Credentials::generate("hive").unwrap();
        assert_ne!(a.password, b.password);
        let (user, rest) = a.rpcauth().split_once(':').unwrap();
        let (salt, hash) = rest.split_once('$').unwrap();
        assert_eq!(user, "hive");
        assert_eq!(salt.len(), 32);
        assert_eq!(hash.len(), 64);
        assert_eq!(a.rpcauth(), rpcauth_value("hive", salt, &a.password));
        assert!(!format!("{a:?}").contains(&a.password));
    }

    #[test]
    fn wallet_names_are_escaped_in_the_path() {
        assert_eq!(percent_encode("hive-spike"), "hive-spike");
        assert_eq!(percent_encode("my wallet/../x"), "my%20wallet%2F..%2Fx");
    }

    #[test]
    fn replies_map_to_results_and_errors() {
        assert_eq!(
            parse_reply(json!({"result": 5, "error": null, "id": "hive"})),
            Ok(json!(5))
        );
        assert_eq!(
            parse_reply(
                json!({"result": null, "error": {"code": -28, "message": "Loading block index…"}})
            ),
            Err(RpcError::Rpc {
                code: RPC_IN_WARMUP,
                message: "Loading block index…".into()
            })
        );
        assert!(matches!(
            parse_reply(json!({"error": {"code": "x"}})),
            Err(RpcError::BadReply(_))
        ));
        assert!(matches!(
            parse_reply(json!({"id": 1})),
            Err(RpcError::BadReply(_))
        ));
    }
}
