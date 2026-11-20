//! A blocking TLS 1.3 client for one seat's HTTPS + SSE API
//! (src/flowmesh2/net/https_sse_api.h; the node's own client is
//! src/flowmesh2/cli/https_client.cpp).
//!
//! A seat's certificate is self-signed, so no certificate authority and no
//! host name is checked. The session trusts exactly one key: the certificate
//! must carry the SubjectPublicKeyInfo whose SHA-256 is the seat's pin, and
//! the handshake must be signed with that key. A peer with any other key is
//! dropped before a byte of the request leaves.
//!
//! One connection per request (`Connection: close`), as the API expects.
//! Every step has a deadline, replies are capped, and a reply counts only
//! when its Content-Length or its chunk terminator says it is complete.

use std::io::{Read, Write};
use std::net::{IpAddr, SocketAddr, TcpStream};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use rustls::client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier};
use rustls::crypto::{verify_tls13_signature, CryptoProvider, WebPkiSupportedAlgorithms};
use rustls::pki_types::{CertificateDer, ServerName, UnixTime};
use rustls::{
    CertificateError, ClientConfig, ClientConnection, DigitallySignedStruct, SignatureScheme,
    StreamOwned,
};
use sha2::{Digest, Sha256};

use super::Error;

/// The largest reply body accepted, and the most undelivered stream text.
const MAX_REPLY_BYTES: usize = 4 * 1024 * 1024;
const MAX_CHUNK_BYTES: usize = 1024 * 1024;
const MAX_HEADER_BYTES: usize = 16 * 1024;
/// How often a stream read wakes to look at its stop flag.
const STREAM_POLL: Duration = Duration::from_millis(250);

/// Where one seat's API is, and the key its certificate must carry.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SeatEndpoint {
    pub seat: u32,
    pub address: IpAddr,
    pub port: u16,
    /// SHA-256 of the certificate's SubjectPublicKeyInfo.
    pub pin: [u8; 32],
}

#[derive(Debug)]
struct PinVerifier {
    pin: [u8; 32],
    algorithms: WebPkiSupportedAlgorithms,
}

impl ServerCertVerifier for PinVerifier {
    fn verify_server_cert(
        &self,
        end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _server_name: &ServerName<'_>,
        _ocsp_response: &[u8],
        _now: UnixTime,
    ) -> Result<ServerCertVerified, rustls::Error> {
        let certificate = webpki::EndEntityCert::try_from(end_entity)
            .map_err(|_| rustls::Error::InvalidCertificate(CertificateError::BadEncoding))?;
        let seen: [u8; 32] = Sha256::digest(certificate.subject_public_key_info().as_ref()).into();
        // Both values are public; the comparison need not be constant-time.
        if seen == self.pin {
            Ok(ServerCertVerified::assertion())
        } else {
            Err(rustls::Error::InvalidCertificate(
                CertificateError::ApplicationVerificationFailure,
            ))
        }
    }

    fn verify_tls12_signature(
        &self,
        _message: &[u8],
        _cert: &CertificateDer<'_>,
        _dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        Err(rustls::Error::General("TLS 1.3 only".into()))
    }

    /// The handshake is signed with the certificate's key: together with the
    /// pin this proves the peer holds the pinned key.
    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        verify_tls13_signature(message, cert, dss, &self.algorithms)
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        self.algorithms.supported_schemes()
    }
}

fn tls_config(pin: [u8; 32]) -> Result<Arc<ClientConfig>, Error> {
    let provider: Arc<CryptoProvider> = Arc::new(rustls::crypto::ring::default_provider());
    let verifier = PinVerifier {
        pin,
        algorithms: provider.signature_verification_algorithms,
    };
    let config = ClientConfig::builder_with_provider(provider)
        .with_protocol_versions(&[&rustls::version::TLS13])
        .map_err(|e| Error::Transport(format!("TLS setup failed: {e}")))?
        .dangerous()
        .with_custom_certificate_verifier(Arc::new(verifier))
        .with_no_client_auth();
    Ok(Arc::new(config))
}

type Tls = StreamOwned<ClientConnection, TcpStream>;

fn transport(seat: &SeatEndpoint, what: &str, error: impl std::fmt::Display) -> Error {
    Error::Transport(format!("seat {}: {what}: {error}", seat.seat))
}

/// A pinned connection with the handshake completed, so a wrong key fails
/// here and nothing of the request is sent to it.
fn connect(seat: &SeatEndpoint, timeout: Duration) -> Result<Tls, Error> {
    let tcp = TcpStream::connect_timeout(&SocketAddr::new(seat.address, seat.port), timeout)
        .map_err(|e| transport(seat, "cannot connect", e))?;
    tcp.set_read_timeout(Some(timeout))
        .map_err(|e| transport(seat, "socket", e))?;
    tcp.set_write_timeout(Some(timeout))
        .map_err(|e| transport(seat, "socket", e))?;
    tcp.set_nodelay(true).ok();
    let connection = ClientConnection::new(
        tls_config(seat.pin)?,
        ServerName::IpAddress(seat.address.into()),
    )
    .map_err(|e| transport(seat, "TLS setup failed", e))?;
    let mut tls = StreamOwned::new(connection, tcp);
    while tls.conn.is_handshaking() {
        tls.conn.complete_io(&mut tls.sock).map_err(|e| {
            let pinned = e
                .get_ref()
                .and_then(|inner| inner.downcast_ref::<rustls::Error>())
                .is_some_and(|inner| {
                    matches!(
                        inner,
                        rustls::Error::InvalidCertificate(
                            CertificateError::ApplicationVerificationFailure
                        )
                    )
                });
            if pinned {
                Error::Verify("the seat's certificate does not carry the pinned key")
            } else {
                transport(seat, "TLS handshake failed", e)
            }
        })?;
    }
    Ok(tls)
}

fn host(seat: &SeatEndpoint) -> String {
    SocketAddr::new(seat.address, seat.port).to_string()
}

/// An incremental decoder of a chunked transfer encoding.
#[derive(Default)]
struct ChunkDecoder {
    pending: Vec<u8>,
    decoded: Vec<u8>,
    /// Bytes of the current chunk still to come, plus its closing CRLF.
    remaining: Option<usize>,
    done: bool,
}

impl ChunkDecoder {
    fn feed(&mut self, bytes: &[u8]) -> Result<(), Error> {
        const MALFORMED: Error = Error::Decode("malformed chunked reply");
        self.pending.extend_from_slice(bytes);
        while !self.done {
            match self.remaining {
                Some(remaining) => {
                    // The chunk's bytes, then its CRLF.
                    if self.pending.len() < remaining + 2 {
                        return Ok(());
                    }
                    if &self.pending[remaining..remaining + 2] != b"\r\n" {
                        return Err(MALFORMED);
                    }
                    self.decoded.extend_from_slice(&self.pending[..remaining]);
                    self.pending.drain(..remaining + 2);
                    self.remaining = None;
                }
                None => {
                    let Some(end) = self.pending.windows(2).position(|pair| pair == b"\r\n") else {
                        if self.pending.len() > 64 {
                            return Err(MALFORMED);
                        }
                        return Ok(());
                    };
                    let line = std::str::from_utf8(&self.pending[..end]).map_err(|_| MALFORMED)?;
                    let size = line.split(';').next().unwrap_or_default().trim();
                    if size.is_empty()
                        || size.len() > 8
                        || !size.bytes().all(|b| b.is_ascii_hexdigit())
                    {
                        return Err(MALFORMED);
                    }
                    let size = usize::from_str_radix(size, 16).map_err(|_| MALFORMED)?;
                    if size > MAX_CHUNK_BYTES {
                        return Err(MALFORMED);
                    }
                    self.pending.drain(..end + 2);
                    if size == 0 {
                        // The terminal chunk; trailers are not used by the API.
                        self.done = true;
                    } else {
                        self.remaining = Some(size);
                    }
                }
            }
        }
        Ok(())
    }
}

#[derive(Debug, PartialEq, Eq)]
enum Framing {
    Length(usize),
    Chunked,
}

/// The status and body framing of a reply's header block.
fn parse_head(head: &str) -> Result<(u16, Framing), Error> {
    const MALFORMED: Error = Error::Decode("malformed HTTP reply");
    let mut lines = head.split("\r\n");
    let status = lines
        .next()
        .and_then(|line| line.strip_prefix("HTTP/1.1 "))
        .ok_or(MALFORMED)?;
    let code = status
        .get(..3)
        .filter(|code| code.bytes().all(|b| b.is_ascii_digit()))
        .ok_or(MALFORMED)?;
    let code: u16 = code.parse().map_err(|_| MALFORMED)?;
    if !(100..=599).contains(&code) || !matches!(status.as_bytes().get(3), None | Some(b' ')) {
        return Err(MALFORMED);
    }
    let mut framing = None;
    for line in lines {
        let Some((name, value)) = line.split_once(':') else {
            continue;
        };
        let value = value.trim();
        if name.eq_ignore_ascii_case("transfer-encoding") && value.eq_ignore_ascii_case("chunked") {
            framing = Some(Framing::Chunked);
        } else if name.eq_ignore_ascii_case("content-length") && framing.is_none() {
            let length: usize = value.parse().map_err(|_| MALFORMED)?;
            framing = Some(Framing::Length(length));
        }
    }
    Ok((code, framing.ok_or(MALFORMED)?))
}

/// Reads until the header block is complete: (header text, bytes after it).
fn read_head(
    tls: &mut Tls,
    seat: &SeatEndpoint,
    deadline: Instant,
) -> Result<(String, Vec<u8>), Error> {
    let mut raw = Vec::new();
    let mut buffer = [0u8; 8192];
    loop {
        if let Some(end) = raw.windows(4).position(|w| w == b"\r\n\r\n") {
            let head = String::from_utf8(raw[..end].to_vec())
                .map_err(|_| Error::Decode("malformed HTTP reply"))?;
            return Ok((head, raw[end + 4..].to_vec()));
        }
        if raw.len() > MAX_HEADER_BYTES {
            return Err(Error::Oversize("HTTP reply header"));
        }
        if Instant::now() >= deadline {
            return Err(transport(seat, "no reply", "timed out"));
        }
        match tls.read(&mut buffer) {
            Ok(0) => return Err(transport(seat, "no reply", "the connection closed")),
            Ok(n) => raw.extend_from_slice(&buffer[..n]),
            Err(e) => return Err(transport(seat, "no reply", e)),
        }
    }
}

pub struct Response {
    pub status: u16,
    pub body: Vec<u8>,
}

/// One request on a fresh pinned connection; the whole reply within `timeout`.
pub fn request(
    seat: &SeatEndpoint,
    method: &str,
    path: &str,
    body: Option<&[u8]>,
    timeout: Duration,
) -> Result<Response, Error> {
    let deadline = Instant::now() + timeout;
    let mut tls = connect(seat, timeout)?;
    let mut head = format!(
        "{method} {path} HTTP/1.1\r\nHost: {}\r\nConnection: close\r\n",
        host(seat)
    );
    if let Some(body) = body {
        head.push_str(&format!(
            "Content-Type: application/octet-stream\r\nContent-Length: {}\r\n",
            body.len()
        ));
    }
    head.push_str("\r\n");
    tls.write_all(head.as_bytes())
        .map_err(|e| transport(seat, "cannot send", e))?;
    if let Some(body) = body {
        tls.write_all(body)
            .map_err(|e| transport(seat, "cannot send", e))?;
    }
    tls.flush().map_err(|e| transport(seat, "cannot send", e))?;

    let (head, rest) = read_head(&mut tls, seat, deadline)?;
    let (status, framing) = parse_head(&head)?;
    let mut chunks = ChunkDecoder::default();
    let mut plain = Vec::new();
    let feed =
        |bytes: &[u8], chunks: &mut ChunkDecoder, plain: &mut Vec<u8>| -> Result<bool, Error> {
            match framing {
                Framing::Chunked => {
                    chunks.feed(bytes)?;
                    if chunks.decoded.len() > MAX_REPLY_BYTES {
                        return Err(Error::Oversize("HTTP reply body"));
                    }
                    Ok(chunks.done)
                }
                Framing::Length(length) => {
                    if length > MAX_REPLY_BYTES {
                        return Err(Error::Oversize("HTTP reply body"));
                    }
                    plain.extend_from_slice(bytes);
                    Ok(plain.len() >= length)
                }
            }
        };
    let mut complete = feed(&rest, &mut chunks, &mut plain)?;
    let mut buffer = [0u8; 16 * 1024];
    while !complete {
        if Instant::now() >= deadline {
            return Err(transport(seat, "incomplete reply", "timed out"));
        }
        match tls.read(&mut buffer) {
            Ok(0) => return Err(transport(seat, "incomplete reply", "the connection closed")),
            Ok(n) => complete = feed(&buffer[..n], &mut chunks, &mut plain)?,
            Err(e) => return Err(transport(seat, "incomplete reply", e)),
        }
    }
    let body = match framing {
        Framing::Chunked => chunks.decoded,
        Framing::Length(length) => {
            plain.truncate(length);
            plain
        }
    };
    Ok(Response { status, body })
}

/// One server-sent event.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct SseEvent {
    pub id: String,
    pub event: String,
    pub data: String,
}

/// Splits decoded stream text into events at blank lines.
#[derive(Default)]
struct SseParser {
    text: Vec<u8>,
}

impl SseParser {
    fn feed(&mut self, bytes: &[u8]) -> Result<Vec<SseEvent>, Error> {
        self.text.extend_from_slice(bytes);
        let mut events = Vec::new();
        while let Some(end) = self.text.windows(2).position(|pair| pair == b"\n\n") {
            let block: Vec<u8> = self.text.drain(..end + 2).collect();
            let block = std::str::from_utf8(&block[..end])
                .map_err(|_| Error::Decode("malformed event stream"))?;
            let mut event = SseEvent::default();
            for line in block.lines() {
                if let Some(value) = line.strip_prefix("id: ") {
                    event.id = value.to_owned();
                } else if let Some(value) = line.strip_prefix("event: ") {
                    event.event = value.to_owned();
                } else if let Some(value) = line.strip_prefix("data: ") {
                    event.data.push_str(value);
                }
            }
            events.push(event);
        }
        if self.text.len() > MAX_REPLY_BYTES {
            return Err(Error::Oversize("stream event"));
        }
        Ok(events)
    }
}

/// How a stream ended without an error.
#[derive(Debug, PartialEq, Eq)]
pub enum StreamEnd {
    /// `stop` was set, or `on_event` returned false.
    Stopped,
    /// The server closed the stream (after a `gap` event, or on its own).
    Closed,
}

/// GET an event stream and hand each event to `on_event` until it returns
/// false, `stop` is set, the server closes, or nothing (not even a
/// heartbeat) arrives for `idle`. A non-200 answer is an error with the
/// server's own words.
pub fn stream(
    seat: &SeatEndpoint,
    path: &str,
    connect_timeout: Duration,
    idle: Duration,
    stop: &AtomicBool,
    mut on_event: impl FnMut(SseEvent) -> bool,
) -> Result<StreamEnd, Error> {
    let mut tls = connect(seat, connect_timeout)?;
    let head = format!(
        "GET {path} HTTP/1.1\r\nHost: {}\r\nAccept: text/event-stream\r\nConnection: close\r\n\r\n",
        host(seat)
    );
    tls.write_all(head.as_bytes())
        .map_err(|e| transport(seat, "cannot send", e))?;
    tls.flush().map_err(|e| transport(seat, "cannot send", e))?;
    let (head, rest) = read_head(&mut tls, seat, Instant::now() + connect_timeout)?;
    let (status, framing) = parse_head(&head)?;
    if status != 200 {
        // The server's refusal is short JSON; show what arrived of it.
        let mut body = rest;
        let mut buffer = [0u8; 4096];
        while body.len() < 4096 {
            match tls.read(&mut buffer) {
                Ok(n) if n > 0 => body.extend_from_slice(&buffer[..n]),
                _ => break,
            }
        }
        let text = String::from_utf8_lossy(&body);
        let reason = text.find('{').map_or("", |start| {
            text[start..].trim_end_matches(['\r', '\n', '0'])
        });
        return Err(Error::Transport(format!(
            "seat {}: the stream was refused (HTTP {status}) {reason}",
            seat.seat
        )));
    }
    if framing != Framing::Chunked {
        return Err(Error::Decode("an event stream is chunked"));
    }
    tls.sock
        .set_read_timeout(Some(STREAM_POLL))
        .map_err(|e| transport(seat, "socket", e))?;
    let mut chunks = ChunkDecoder::default();
    let mut parser = SseParser::default();
    let mut buffer = [0u8; 16 * 1024];
    let mut last_data = Instant::now();
    let mut pending: &[u8] = &rest;
    loop {
        chunks.feed(pending)?;
        let decoded = std::mem::take(&mut chunks.decoded);
        for event in parser.feed(&decoded)? {
            if !on_event(event) {
                return Ok(StreamEnd::Stopped);
            }
        }
        if chunks.done {
            return Ok(StreamEnd::Closed);
        }
        pending = loop {
            if stop.load(Ordering::Relaxed) {
                return Ok(StreamEnd::Stopped);
            }
            match tls.read(&mut buffer) {
                Ok(0) => return Ok(StreamEnd::Closed),
                Ok(n) => {
                    last_data = Instant::now();
                    break &buffer[..n];
                }
                Err(e)
                    if matches!(
                        e.kind(),
                        std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
                    ) =>
                {
                    if last_data.elapsed() >= idle {
                        return Err(transport(seat, "the stream went silent", "no heartbeat"));
                    }
                }
                Err(e) => return Err(transport(seat, "the stream broke", e)),
            }
        };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn chunks_decode_across_any_split() {
        let wire = b"5\r\nhello\r\n1;ext=1\r\n \r\n6\r\nworld!\r\n0\r\n\r\n";
        for split in 0..wire.len() {
            let mut decoder = ChunkDecoder::default();
            decoder.feed(&wire[..split]).unwrap();
            decoder.feed(&wire[split..]).unwrap();
            assert!(decoder.done, "split at {split}");
            assert_eq!(decoder.decoded, b"hello world!");
        }
    }

    #[test]
    fn malformed_chunks_are_refused() {
        for wire in [
            &b"zz\r\nhello\r\n"[..],
            b"5\r\nhelloXX",
            b"100001\r\n",
            b"123456789\r\n",
            b"\r\n",
        ] {
            assert!(ChunkDecoder::default().feed(wire).is_err(), "{wire:?}");
        }
        // A size line that never ends.
        assert!(ChunkDecoder::default().feed(&[b'1'; 80]).is_err());
    }

    #[test]
    fn a_reply_head_gives_status_and_framing() {
        assert_eq!(
            parse_head("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 12"),
            Ok((200, Framing::Length(12)))
        );
        assert_eq!(
            parse_head("HTTP/1.1 429 Too Many\r\ntransfer-encoding: Chunked"),
            Ok((429, Framing::Chunked))
        );
        assert!(parse_head("HTTP/1.0 200 OK\r\nContent-Length: 1").is_err());
        assert!(parse_head("HTTP/1.1 2000 OK\r\nContent-Length: 1").is_err());
        assert!(parse_head("HTTP/1.1 200 OK\r\nContent-Length: x").is_err());
        // Neither a length nor chunks: the end of the body would be a guess.
        assert!(parse_head("HTTP/1.1 200 OK\r\nServer: x").is_err());
    }

    #[test]
    fn events_split_at_blank_lines_across_reads() {
        let text = b"id: 7:1:40\nevent: snapshot\ndata: {\"a\":1}\n\nevent: heartbeat\ndata: {}\n\nid: 7:2";
        for split in 0..text.len() {
            let mut parser = SseParser::default();
            let mut events = parser.feed(&text[..split]).unwrap();
            events.extend(parser.feed(&text[split..]).unwrap());
            assert_eq!(events.len(), 2, "split at {split}");
            assert_eq!(
                events[0],
                SseEvent {
                    id: "7:1:40".into(),
                    event: "snapshot".into(),
                    data: "{\"a\":1}".into()
                }
            );
            assert_eq!(
                events[1],
                SseEvent {
                    id: String::new(),
                    event: "heartbeat".into(),
                    data: "{}".into()
                }
            );
        }
    }
}
