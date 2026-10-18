//! Drives the real b3-hive-update helper with a scripted network: no
//! sockets, real signatures. Runs only when both tools are named:
//!   B3_HIVE_UPDATE=/path/to/b3-hive-update B3_HIVE_SIGN=/path/to/b3hive-sign \
//!     cargo test --test update_helper
//! The helper must be a developer build (no pinned channel), which takes
//! the channel on its command line.

use std::collections::HashMap;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{SystemTime, UNIX_EPOCH};

use sha2::{Digest, Sha256};

use b3_hive::update::{Fetcher, Response, Updater};

const MANIFEST_URL: &str = "https://rel.example/manifest.txt";
const ARTIFACT_URL: &str = "https://rel.example/hive-2.0.0";
const CDN_URL: &str = "https://cdn.example/hive-2.0.0";

/// Answers from a table; a body is handed out in small reads, as a socket would.
struct Scripted(HashMap<&'static str, Result<Answer, &'static str>>);

#[derive(Clone)]
enum Answer {
    Redirect(&'static str),
    Body(Vec<u8>),
}

struct Trickle(Vec<u8>, usize);

impl Read for Trickle {
    fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
        let n = buf.len().min(7_001).min(self.0.len() - self.1);
        buf[..n].copy_from_slice(&self.0[self.1..self.1 + n]);
        self.1 += n;
        Ok(n)
    }
}

impl Fetcher for Scripted {
    fn open(&self, url: &str) -> Result<Response, String> {
        match self.0.get(url).cloned() {
            Some(Ok(Answer::Redirect(next))) => Ok(Response::Redirect(next.to_owned())),
            Some(Ok(Answer::Body(bytes))) => Ok(Response::Body(Box::new(Trickle(bytes, 0)))),
            Some(Err(token)) => Err(token.to_owned()),
            None => Err("update-fetch-http-404".to_owned()),
        }
    }
}

struct Tools {
    helper: PathBuf,
    sign: PathBuf,
    dir: PathBuf,
}

impl Tools {
    fn get(name: &str) -> Option<Self> {
        let (Some(helper), Some(sign)) = (
            std::env::var_os("B3_HIVE_UPDATE"),
            std::env::var_os("B3_HIVE_SIGN"),
        ) else {
            eprintln!("skipped: B3_HIVE_UPDATE and B3_HIVE_SIGN are not both set");
            return None;
        };
        let dir = std::env::temp_dir().join(format!("hive-update-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        Some(Self {
            helper: helper.into(),
            sign: sign.into(),
            dir,
        })
    }

    fn run(&self, args: &[&Path]) -> String {
        let out = Command::new(&self.sign).args(args).output().unwrap();
        assert!(
            out.status.success(),
            "{}",
            String::from_utf8_lossy(&out.stderr)
        );
        // b3hive-sign.exe prints through a text-mode stdout: CRLF on Windows.
        String::from_utf8(out.stdout).unwrap().replace("\r\n", "\n")
    }

    /// A fresh release key; returns its file and x-only public key.
    fn key(&self, name: &str) -> (PathBuf, String) {
        let file = self.dir.join(name);
        let out = self.run(&[Path::new("genkey"), &file]);
        let pubkey = out
            .lines()
            .find_map(|l| l.strip_prefix("pubkey="))
            .unwrap()
            .to_owned();
        (file, pubkey)
    }

    /// The payload signed by every key, as a manifest file.
    fn manifest(&self, payload: &str, keys: &[&Path]) -> Vec<u8> {
        let file = self.dir.join("payload");
        std::fs::write(&file, payload).unwrap();
        let mut manifest = format!("{payload}-----SIGNATURES-----\n");
        for key in keys {
            manifest += &self.run(&[Path::new("sign"), &file, key]);
        }
        manifest.into_bytes()
    }

    fn helper(&self, pubkeys: &[&str]) -> Updater {
        let mut args = vec![
            format!("--state-dir={}", self.dir.join("state").display()),
            format!("--manifest-url={MANIFEST_URL}"),
            "--threshold=2".to_owned(),
            "--host=rel.example".to_owned(),
            "--host=cdn.example".to_owned(),
            "--installed-version=1.0.0".to_owned(),
        ];
        args.extend(pubkeys.iter().map(|k| format!("--key={k}")));
        Updater::start(&self.helper, &args).unwrap()
    }
}

impl Drop for Tools {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.dir);
    }
}

fn now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_secs()
}

fn payload(sequence: u64, artifact: &[u8]) -> String {
    let (os, format) = if cfg!(windows) {
        ("windows", "exe")
    } else if cfg!(target_os = "macos") {
        ("macos", "zip")
    } else {
        ("linux", "targz")
    };
    let arch = if cfg!(target_arch = "aarch64") {
        "arm64"
    } else {
        "x86_64"
    };
    let sha = hex(&Sha256::digest(artifact));
    let notes = hex(&Sha256::digest(b"notes"));
    let published = now() - 60;
    let expires = now() + 86_400;
    format!(
        "B3-HIVE-MANIFEST-V1\nchannel=stable\nsequence={sequence}\npublished={published}\n\
         expires={expires}\nnotes_sha256={notes}\nartifact.os={os}\nartifact.arch={arch}\n\
         artifact.format={format}\nartifact.version=2.0.0\nartifact.size={}\n\
         artifact.sha256={sha}\nartifact.url={ARTIFACT_URL}\n",
        artifact.len()
    )
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

#[test]
fn checks_and_downloads_a_signed_release() {
    let Some(tools) = Tools::get("flow") else {
        return;
    };
    let (k1, p1) = tools.key("k1");
    let (k2, p2) = tools.key("k2");
    let artifact: Vec<u8> = (0..150_000u32).map(|i| (i % 251) as u8).collect();
    let manifest = tools.manifest(&payload(3, &artifact), &[&k1, &k2]);
    let net = Scripted(HashMap::from([
        (MANIFEST_URL, Ok(Answer::Body(manifest))),
        (ARTIFACT_URL, Ok(Answer::Redirect(CDN_URL))),
        (CDN_URL, Ok(Answer::Body(artifact.clone()))),
    ]));

    let mut updater = tools.helper(&[&p1, &p2]);
    assert_eq!(updater.status().unwrap().state, "idle");
    let checked = updater.check(&net).unwrap();
    assert_eq!(checked.state, "update-available", "{checked:?}");
    let offer = checked.offer.unwrap();
    assert_eq!(offer.version, "2.0.0");
    assert_eq!(offer.size, artifact.len() as u64);
    assert_eq!(offer.sha256, hex(&Sha256::digest(&artifact)));

    let mut seen = Vec::new();
    let done = updater.download(&net, &mut |n| seen.push(n)).unwrap();
    assert_eq!(done.state, "ready-to-install", "{done:?}");
    assert_eq!(seen.last().copied(), Some(artifact.len() as u64));
    assert!(seen.windows(2).all(|w| w[0] < w[1]));
    let saved = std::fs::read(done.download.unwrap()).unwrap();
    assert!(saved == artifact);
    // The rollback floor is on disk in the Qt record format.
    let record = std::fs::read_to_string(tools.dir.join("state").join("sequence")).unwrap();
    assert!(record.starts_with("v1:3:"), "{record}");
}

#[test]
fn refuses_what_the_policy_does_not_allow() {
    let Some(tools) = Tools::get("refuse") else {
        return;
    };
    let (k1, p1) = tools.key("k1");
    let (k2, p2) = tools.key("k2");
    let artifact = b"release".to_vec();

    // One signature where two are required.
    let net = Scripted(HashMap::from([(
        MANIFEST_URL,
        Ok(Answer::Body(tools.manifest(&payload(1, &artifact), &[&k1]))),
    )]));
    let mut updater = tools.helper(&[&p1, &p2]);
    let r = updater.check(&net).unwrap();
    assert_eq!(r.state, "failed");
    assert!(r.error.starts_with("update-"), "{r:?}");
    assert!(r.offer.is_none());

    // A redirect off the approved hosts is denied, and the pipe stays usable.
    let net = Scripted(HashMap::from([(
        MANIFEST_URL,
        Ok(Answer::Redirect("https://evil.example/manifest.txt")),
    )]));
    let r = updater.check(&net).unwrap();
    assert_eq!(
        (r.state.as_str(), r.error.as_str()),
        ("failed", "update-fetch-redirect")
    );

    // Network failures arrive as their token; others are replaced.
    for (sent, expected) in [
        ("update-fetch-tls", "update-fetch-tls"),
        ("update-manifest-signature", "update-fetch-failed"),
    ] {
        let net = Scripted(HashMap::from([(MANIFEST_URL, Err(sent))]));
        let r = updater.check(&net).unwrap();
        assert_eq!(r.error, expected);
    }

    // An artifact that differs from the signed digest is not kept.
    let net = Scripted(HashMap::from([
        (
            MANIFEST_URL,
            Ok(Answer::Body(
                tools.manifest(&payload(2, &artifact), &[&k1, &k2]),
            )),
        ),
        (ARTIFACT_URL, Ok(Answer::Body(b"releasE".to_vec()))),
    ]));
    assert_eq!(updater.check(&net).unwrap().state, "update-available");
    let r = updater.download(&net, &mut |_| {}).unwrap();
    assert_eq!(r.state, "failed", "{r:?}");
    assert!(r.download.is_none());
    assert!(updater.alive());
}
