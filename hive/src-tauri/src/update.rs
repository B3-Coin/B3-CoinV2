//! App updates through b3-hive-update, the C++ helper that runs the same
//! update manager as the Qt client (doc/design/b3-hive-update-system.md).
//!
//! The helper decides everything: the pinned channel and keys, approved
//! hosts, redirects, size limits, digests, signatures and the rollback floor.
//! This side only carries bytes. It opens HTTPS connections for the helper,
//! reports every redirect before following it, and streams bodies in frames
//! the helper accepts one by one (src/update/pipe.h has the protocol).

use std::io::{BufRead, BufReader, Read, Write};
use std::path::PathBuf;
use std::process::{Child, ChildStdin, ChildStdout, Command, Stdio};
use std::time::Duration;

use serde::{Deserialize, Serialize};

/// Largest body frame the helper accepts (MAX_PIPE_CHUNK).
const CHUNK: usize = 64 * 1024;
/// Longest line the helper writes, a RESULT included.
const MAX_LINE: u64 = 64 * 1024;

/// What the helper reports after every command.
#[derive(Clone, Debug, Default, Deserialize, Serialize, PartialEq, Eq)]
pub struct UpdateStatus {
    pub state: String,
    pub error: String,
    pub installed: String,
    pub offer: Option<Offer>,
    pub download: Option<String>,
}

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
pub struct Offer {
    pub version: String,
    pub size: u64,
    pub sha256: String,
    pub url: String,
    pub notes_sha256: String,
}

/// One answer from the network for one URL.
pub enum Response {
    /// A 3xx with a Location, resolved to an absolute URL.
    Redirect(String),
    /// A 200 body.
    Body(Box<dyn Read + Send>),
}

/// Opens one URL without following redirects. Errors are helper tokens
/// ("update-fetch-…"); the helper replaces anything else.
pub trait Fetcher: Send {
    fn open(&self, url: &str) -> Result<Response, String>;
}

/// HTTPS with rustls and the bundled webpki roots, no redirects followed,
/// no content decoding: the helper hashes exactly the bytes it is sent.
pub struct HttpsFetcher {
    agent: ureq::Agent,
}

impl HttpsFetcher {
    pub fn new() -> Self {
        let agent = ureq::Agent::config_builder()
            .https_only(true)
            .max_redirects(0)
            .http_status_as_error(false)
            .user_agent("B3Hive-Update")
            .timeout_connect(Some(Duration::from_secs(30)))
            .timeout_recv_response(Some(Duration::from_secs(30)))
            // A release artifact over a slow line, but never forever.
            .timeout_global(Some(Duration::from_secs(30 * 60)))
            .build()
            .into();
        Self { agent }
    }
}

impl Default for HttpsFetcher {
    fn default() -> Self {
        Self::new()
    }
}

impl Fetcher for HttpsFetcher {
    fn open(&self, url: &str) -> Result<Response, String> {
        let response = self.agent.get(url).call().map_err(fetch_error)?;
        let status = response.status().as_u16();
        match status {
            200 => Ok(Response::Body(Box::new(response.into_body().into_reader()))),
            301 | 302 | 303 | 307 | 308 => {
                let location = response
                    .headers()
                    .get("location")
                    .and_then(|v| v.to_str().ok())
                    .ok_or("update-fetch-redirect")?;
                resolve(url, location)
                    .map(Response::Redirect)
                    .ok_or_else(|| "update-fetch-redirect".to_owned())
            }
            _ => Err(format!("update-fetch-http-{status}")),
        }
    }
}

fn fetch_error(e: ureq::Error) -> String {
    match e {
        ureq::Error::Timeout(_) => "update-fetch-timeout",
        ureq::Error::HostNotFound => "update-fetch-dns",
        ureq::Error::ConnectionFailed => "update-fetch-connect",
        ureq::Error::Tls(_) | ureq::Error::Rustls(_) => "update-fetch-tls",
        ureq::Error::TlsRequired => "update-fetch-scheme",
        _ => "update-fetch-failed",
    }
    .to_owned()
}

/// Resolves a Location header against the URL that returned it. Only
/// absolute, scheme-relative and path-absolute forms are taken; the helper
/// then judges the result like any other redirect.
fn resolve(base: &str, location: &str) -> Option<String> {
    if location.starts_with("https://") || location.starts_with("http://") {
        return Some(location.to_owned());
    }
    let (scheme, rest) = base.split_once("://")?;
    if let Some(authority_path) = location.strip_prefix("//") {
        return Some(format!("{scheme}://{authority_path}"));
    }
    if location.starts_with('/') {
        let authority = rest.split(['/', '?', '#']).next()?;
        return Some(format!("{scheme}://{authority}{location}"));
    }
    None
}

/// A running helper. Commands block until the helper answers, serving its
/// fetches meanwhile; `progress` hears the body bytes delivered so far.
pub struct Updater {
    child: Child,
    to_helper: ChildStdin,
    from_helper: BufReader<ChildStdout>,
}

impl Updater {
    pub fn start(helper: &PathBuf, args: &[String]) -> Result<Self, String> {
        let mut command = Command::new(helper);
        command
            .args(args)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null());
        crate::node::hide_console(&mut command);
        let mut child = command
            .spawn()
            .map_err(|e| format!("cannot start {}: {e}", helper.display()))?;
        let to_helper = child.stdin.take().ok_or("no helper stdin")?;
        let from_helper = BufReader::new(child.stdout.take().ok_or("no helper stdout")?);
        Ok(Self {
            child,
            to_helper,
            from_helper,
        })
    }

    pub fn status(&mut self) -> Result<UpdateStatus, String> {
        self.command("STATUS", &NoFetch, &mut |_| {})
    }

    pub fn check(&mut self, fetcher: &dyn Fetcher) -> Result<UpdateStatus, String> {
        self.command("CHECK", fetcher, &mut |_| {})
    }

    pub fn download(
        &mut self,
        fetcher: &dyn Fetcher,
        progress: &mut dyn FnMut(u64),
    ) -> Result<UpdateStatus, String> {
        self.command("DOWNLOAD", fetcher, progress)
    }

    /// True while the helper process runs.
    pub fn alive(&mut self) -> bool {
        matches!(self.child.try_wait(), Ok(None))
    }

    fn command(
        &mut self,
        name: &str,
        fetcher: &dyn Fetcher,
        progress: &mut dyn FnMut(u64),
    ) -> Result<UpdateStatus, String> {
        self.send(format!("{name}\n").as_bytes())?;
        loop {
            let line = self.read_line()?;
            if let Some(json) = line.strip_prefix("RESULT ") {
                return serde_json::from_str(json)
                    .map_err(|e| format!("unreadable helper result: {e}"));
            }
            let Some(url) = line.strip_prefix("FETCH ") else {
                return Err(format!("unexpected helper line: {line:.80}"));
            };
            self.serve(url.to_owned(), fetcher, progress)?;
        }
    }

    /// One fetch for the helper, until it has the body, refuses a redirect
    /// or a chunk, or hears why the network failed.
    fn serve(
        &mut self,
        mut url: String,
        fetcher: &dyn Fetcher,
        progress: &mut dyn FnMut(u64),
    ) -> Result<(), String> {
        loop {
            let mut body = match fetcher.open(&url) {
                Ok(Response::Redirect(next)) => {
                    self.send(format!("REDIRECT {next}\n").as_bytes())?;
                    if self.read_line()? != "ALLOW" {
                        return Ok(());
                    }
                    url = next;
                    continue;
                }
                Ok(Response::Body(body)) => body,
                Err(token) => return self.send(format!("FAIL {token}\n").as_bytes()),
            };
            let mut buffer = vec![0u8; CHUNK];
            let mut delivered = 0u64;
            loop {
                let n = match body.read(&mut buffer) {
                    Ok(0) => return self.send(b"DONE\n"),
                    Ok(n) => n,
                    Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
                    Err(e) => {
                        let token = if e.kind() == std::io::ErrorKind::TimedOut {
                            "update-fetch-timeout"
                        } else {
                            "update-fetch-read"
                        };
                        return self.send(format!("FAIL {token}\n").as_bytes());
                    }
                };
                let mut frame = format!("DATA {n}\n").into_bytes();
                frame.extend_from_slice(&buffer[..n]);
                self.send(&frame)?;
                if self.read_line()? != "MORE" {
                    return Ok(());
                }
                delivered += n as u64;
                progress(delivered);
            }
        }
    }

    fn send(&mut self, bytes: &[u8]) -> Result<(), String> {
        self.to_helper
            .write_all(bytes)
            .and_then(|()| self.to_helper.flush())
            .map_err(|e| format!("the update helper stopped: {e}"))
    }

    fn read_line(&mut self) -> Result<String, String> {
        let mut line = String::new();
        let n = (&mut self.from_helper)
            .take(MAX_LINE)
            .read_line(&mut line)
            .map_err(|e| format!("the update helper stopped: {e}"))?;
        if n == 0 || !line.ends_with('\n') {
            return Err("the update helper stopped".to_owned());
        }
        line.pop();
        Ok(line)
    }
}

impl Drop for Updater {
    /// End of input ends the helper; it holds no state worth waiting for
    /// beyond the rollback record, which it writes before answering.
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

/// STATUS never fetches.
struct NoFetch;

impl Fetcher for NoFetch {
    fn open(&self, _url: &str) -> Result<Response, String> {
        Err("update-fetch-failed".to_owned())
    }
}

#[cfg(test)]
mod tests {
    use super::resolve;

    #[test]
    fn locations_resolve_against_the_url_that_sent_them() {
        let base = "https://github.com/B3-Coin/x/releases/download/v1/a.zip?x=1";
        assert_eq!(
            resolve(base, "https://objects.example/a").as_deref(),
            Some("https://objects.example/a")
        );
        assert_eq!(
            resolve(base, "//cdn.example/a").as_deref(),
            Some("https://cdn.example/a")
        );
        assert_eq!(
            resolve(base, "/other/path").as_deref(),
            Some("https://github.com/other/path")
        );
        assert_eq!(
            resolve("https://h.example?q", "/p").as_deref(),
            Some("https://h.example/p")
        );
        assert_eq!(resolve(base, "relative/path"), None);
        assert_eq!(resolve(base, ""), None);
    }
}
