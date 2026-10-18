//! Chain bootstraps from the B3 explorer, for a fresh mainnet data folder.
//!
//! The explorer publishes a manifest of recent chain snapshots (tar.zst of
//! blocks/, chainstate/, indexes/ and flowmesh/) with each archive's size and
//! SHA-256. The manifest is not signed: trust rests on HTTPS to the explorer,
//! and the setup wizard says so. An archive is used only if its size and
//! SHA-256 match the manifest, and it is unpacked only into a data folder
//! that has no chain yet. Unpacking accepts plain files and folders under
//! those four top-level folders and nothing else, so wallets, settings and
//! anything outside the chain folders are never written.

use std::fs::{self, File};
use std::io::{self, Read, Write};
use std::path::{Component, Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

/// The only place bootstraps come from.
pub const ORIGIN: &str = "https://explorer.b3hive.io";
pub const MANIFEST_URL: &str = "https://explorer.b3hive.io/bootstraps/manifest.json";

/// The folders a bootstrap may create, all inside the chain folder.
pub const CHAIN_FOLDERS: [&str; 4] = ["blocks", "chainstate", "indexes", "flowmesh"];

const MAX_MANIFEST: u64 = 1024 * 1024;
/// Far above today's 1 GB archives, but a bound on what a server can make us store.
const MAX_ARCHIVE: u64 = 64 * 1024 * 1024 * 1024;
const MAX_UNPACKED: u64 = 256 * 1024 * 1024 * 1024;
const STAGING: &str = ".bootstrap-staging";
const CHUNK: usize = 1024 * 1024;

/// One archive the explorer offers.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Offer {
    pub height: u64,
    pub size: u64,
    pub sha256: String,
    /// Absolute https URL on ORIGIN.
    pub url: String,
    pub block_hash: Option<String>,
    pub created: Option<String>,
}

#[derive(Deserialize)]
struct RawManifest {
    #[serde(default)]
    bootstraps: Vec<RawEntry>,
}

#[derive(Deserialize)]
struct RawEntry {
    height: Option<u64>,
    size: Option<u64>,
    sha256: Option<String>,
    url: Option<String>,
    block_hash: Option<String>,
    created_utc: Option<String>,
}

fn is_hex64(s: &str) -> bool {
    s.len() == 64 && s.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'))
}

/// `/bootstraps/bootstrap-<height>.tar.zst`, the only path shape accepted.
fn valid_path(path: &str, height: u64) -> bool {
    path == format!("/bootstraps/bootstrap-{height}.tar.zst")
}

/// The usable offers, newest first. Entries that do not fully check out are
/// left out rather than trusted in part.
pub fn parse_manifest(body: &[u8]) -> Result<Vec<Offer>, String> {
    let raw: RawManifest = serde_json::from_slice(body)
        .map_err(|e| format!("The explorer's bootstrap list is not valid: {e}"))?;
    let mut offers: Vec<Offer> = raw
        .bootstraps
        .into_iter()
        .filter_map(|e| {
            let height = e.height.filter(|h| *h > 0)?;
            let size = e.size.filter(|s| *s > 0 && *s <= MAX_ARCHIVE)?;
            let sha256 = e.sha256.filter(|s| is_hex64(s))?;
            let path = e.url.filter(|u| valid_path(u, height))?;
            Some(Offer {
                height,
                size,
                sha256,
                url: format!("{ORIGIN}{path}"),
                block_hash: e.block_hash.filter(|h| is_hex64(h)),
                created: e.created_utc,
            })
        })
        .collect();
    offers.sort_by_key(|o| std::cmp::Reverse(o.height));
    offers.dedup_by_key(|o| o.height);
    Ok(offers)
}

fn empty_or_missing(dir: &Path) -> bool {
    match fs::read_dir(dir) {
        Ok(mut entries) => entries.next().is_none(),
        Err(e) => e.kind() == io::ErrorKind::NotFound,
    }
}

/// True when the chain folder holds no chain: blocks/ and chainstate/ are
/// both missing or empty. Only then may a bootstrap be installed.
pub fn is_fresh(chain_dir: &Path) -> bool {
    empty_or_missing(&chain_dir.join("blocks")) && empty_or_missing(&chain_dir.join("chainstate"))
}

#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(tag = "phase", rename_all = "lowercase")]
pub enum Progress {
    Downloading { received: u64, size: u64 },
    Verifying,
    Unpacking { written: u64 },
    Done { height: u64 },
}

const CANCELLED: &str = "The bootstrap was cancelled.";

fn check_cancel(cancel: &AtomicBool) -> Result<(), String> {
    if cancel.load(Ordering::SeqCst) {
        Err(CANCELLED.into())
    } else {
        Ok(())
    }
}

/// Copies `source` to a new file at `dest`, hashing as it goes, and keeps it
/// only if it is exactly `offer.size` bytes with `offer.sha256`.
pub fn download(
    source: &mut dyn Read,
    offer: &Offer,
    dest: &Path,
    progress: &mut dyn FnMut(Progress),
    cancel: &AtomicBool,
) -> Result<(), String> {
    let mut file =
        File::create_new(dest).map_err(|e| format!("Cannot create {}: {e}", dest.display()))?;
    let result = (|| {
        let mut hasher = Sha256::new();
        let mut buf = vec![0u8; CHUNK];
        let mut received = 0u64;
        progress(Progress::Downloading {
            received,
            size: offer.size,
        });
        loop {
            check_cancel(cancel)?;
            let n = source
                .read(&mut buf)
                .map_err(|e| format!("The download stopped: {e}"))?;
            if n == 0 {
                break;
            }
            received += n as u64;
            if received > offer.size {
                return Err(
                    "The explorer sent more data than its list says the bootstrap has.".into(),
                );
            }
            hasher.update(&buf[..n]);
            file.write_all(&buf[..n])
                .map_err(|e| format!("Cannot write {}: {e}", dest.display()))?;
            progress(Progress::Downloading {
                received,
                size: offer.size,
            });
        }
        progress(Progress::Verifying);
        if received != offer.size {
            return Err(format!(
                "The download ended early: {received} of {} bytes.",
                offer.size
            ));
        }
        let digest = hasher.finalize();
        let hex: String = digest.iter().map(|b| format!("{b:02x}")).collect();
        if hex != offer.sha256 {
            return Err("The download does not match the SHA-256 in the explorer's list.".into());
        }
        file.sync_all()
            .map_err(|e| format!("Cannot write {}: {e}", dest.display()))
    })();
    if result.is_err() {
        drop(file);
        let _ = fs::remove_file(dest);
    }
    result
}

/// The archive path's components, if it is a relative path inside one of
/// the chain folders; None for anything else.
fn safe_relative(path: &Path) -> Option<PathBuf> {
    let mut out = PathBuf::new();
    for (i, component) in path.components().enumerate() {
        match component {
            Component::Normal(part) => {
                let part = part.to_str()?;
                if i == 0 && !CHAIN_FOLDERS.contains(&part) {
                    return None;
                }
                out.push(part);
            }
            Component::CurDir if i == 0 => {}
            _ => return None,
        }
    }
    (!out.as_os_str().is_empty()).then_some(out)
}

/// Unpacks a verified archive into `staging`, refusing the whole archive at
/// the first entry that is not a plain file or folder inside a chain folder.
fn unpack(
    archive: &Path,
    staging: &Path,
    progress: &mut dyn FnMut(Progress),
    cancel: &AtomicBool,
) -> Result<(), String> {
    let file = File::open(archive).map_err(|e| format!("Cannot open the bootstrap: {e}"))?;
    let decoder = zstd::stream::read::Decoder::new(file)
        .map_err(|e| format!("Cannot read the bootstrap: {e}"))?;
    let mut tar = tar::Archive::new(decoder);
    let entries = tar
        .entries()
        .map_err(|e| format!("Cannot read the bootstrap: {e}"))?;
    let mut written = 0u64;
    let mut buf = vec![0u8; CHUNK];
    progress(Progress::Unpacking { written });
    for entry in entries {
        check_cancel(cancel)?;
        let mut entry = entry.map_err(|e| format!("The bootstrap is damaged: {e}"))?;
        let path = entry
            .path()
            .map_err(|e| format!("The bootstrap is damaged: {e}"))?
            .into_owned();
        let relative = safe_relative(&path).ok_or_else(|| {
            format!(
                "The bootstrap contains {}, which is outside the chain folders. Nothing was installed.",
                path.display()
            )
        })?;
        let target = staging.join(&relative);
        match entry.header().entry_type() {
            tar::EntryType::Directory => {
                fs::create_dir_all(&target)
                    .map_err(|e| format!("Cannot create {}: {e}", target.display()))?;
            }
            // GNU tar stores preallocated block files as sparse entries;
            // reading one gives the full file, holes as zeros.
            tar::EntryType::Regular | tar::EntryType::Continuous | tar::EntryType::GNUSparse => {
                if let Some(parent) = target.parent() {
                    fs::create_dir_all(parent)
                        .map_err(|e| format!("Cannot create {}: {e}", parent.display()))?;
                }
                // create_new: a repeated path in the archive is refused, not overwritten.
                let mut out = File::create_new(&target)
                    .map_err(|e| format!("Cannot create {}: {e}", target.display()))?;
                loop {
                    check_cancel(cancel)?;
                    let n = entry
                        .read(&mut buf)
                        .map_err(|e| format!("The bootstrap is damaged: {e}"))?;
                    if n == 0 {
                        break;
                    }
                    written += n as u64;
                    if written > MAX_UNPACKED {
                        return Err("The bootstrap unpacks to more than B3 Hive allows.".into());
                    }
                    out.write_all(&buf[..n])
                        .map_err(|e| format!("Cannot write {}: {e}", target.display()))?;
                    progress(Progress::Unpacking { written });
                }
                out.sync_all()
                    .map_err(|e| format!("Cannot write {}: {e}", target.display()))?;
            }
            other => {
                return Err(format!(
                    "The bootstrap contains {} of a kind B3 Hive does not unpack ({other:?}). Nothing was installed.",
                    path.display()
                ))
            }
        }
    }
    Ok(())
}

/// Moves each unpacked chain folder into the chain folder. A target that
/// exists must be empty: the chain folder was checked to be fresh, and a
/// folder that appeared since is never replaced.
fn move_into_place(staging: &Path, chain_dir: &Path) -> Result<(), String> {
    for name in CHAIN_FOLDERS {
        let from = staging.join(name);
        if !from.exists() {
            continue;
        }
        let to = chain_dir.join(name);
        if to.exists() {
            if !empty_or_missing(&to) {
                return Err(format!(
                    "{} is no longer empty, so the bootstrap was not installed.",
                    to.display()
                ));
            }
            fs::remove_dir(&to).map_err(|e| format!("Cannot replace {}: {e}", to.display()))?;
        }
        fs::rename(&from, &to).map_err(|e| format!("Cannot move {}: {e}", to.display()))?;
    }
    Ok(())
}

/// Installs `offer` into a fresh chain folder: downloads it next to the
/// chain, verifies it, unpacks it into a staging folder, and moves the chain
/// folders into place. On any failure the chain folder is left without the
/// bootstrap (and still fresh), and the download and staging are removed.
pub fn install(
    source: &mut dyn Read,
    offer: &Offer,
    chain_dir: &Path,
    progress: &mut dyn FnMut(Progress),
    cancel: &AtomicBool,
) -> Result<(), String> {
    if !is_fresh(chain_dir) {
        return Err(
            "This data folder already has a chain, so a bootstrap is not installed over it.".into(),
        );
    }
    fs::create_dir_all(chain_dir)
        .map_err(|e| format!("Cannot create {}: {e}", chain_dir.display()))?;
    let archive = chain_dir.join(format!("bootstrap-{}.tar.zst.part", offer.height));
    let staging = chain_dir.join(STAGING);
    // Leftovers from an earlier attempt that was killed.
    let _ = fs::remove_file(&archive);
    let _ = fs::remove_dir_all(&staging);

    let result = (|| {
        download(source, offer, &archive, progress, cancel)?;
        fs::create_dir(&staging)
            .map_err(|e| format!("Cannot create {}: {e}", staging.display()))?;
        unpack(&archive, &staging, progress, cancel)?;
        check_cancel(cancel)?;
        move_into_place(&staging, chain_dir)
    })();
    let _ = fs::remove_file(&archive);
    let _ = fs::remove_dir_all(&staging);
    if result.is_ok() {
        progress(Progress::Done {
            height: offer.height,
        });
    }
    result
}

/// HTTPS to the explorer only, without redirects.
pub struct Explorer {
    agent: ureq::Agent,
}

impl Default for Explorer {
    fn default() -> Self {
        Self::new()
    }
}

impl Explorer {
    pub fn new() -> Self {
        let agent = ureq::Agent::config_builder()
            .https_only(true)
            .max_redirects(0)
            .http_status_as_error(false)
            .user_agent("B3Hive-Bootstrap")
            .timeout_connect(Some(Duration::from_secs(30)))
            .timeout_recv_response(Some(Duration::from_secs(60)))
            // About 1 GB over a slow line, but never forever.
            .timeout_global(Some(Duration::from_secs(12 * 3600)))
            .build()
            .into();
        Self { agent }
    }

    fn get(&self, url: &str) -> Result<ureq::Body, String> {
        if !url.starts_with(&format!("{ORIGIN}/")) {
            return Err("Bootstraps come only from the B3 explorer.".into());
        }
        let response = self
            .agent
            .get(url)
            .call()
            .map_err(|e| format!("The explorer could not be reached: {e}"))?;
        let status = response.status().as_u16();
        if status != 200 {
            return Err(format!("The explorer answered HTTP {status}."));
        }
        Ok(response.into_body())
    }

    pub fn offers(&self) -> Result<Vec<Offer>, String> {
        let mut body = Vec::new();
        self.get(MANIFEST_URL)?
            .into_reader()
            .take(MAX_MANIFEST + 1)
            .read_to_end(&mut body)
            .map_err(|e| format!("The explorer's bootstrap list could not be read: {e}"))?;
        if body.len() as u64 > MAX_MANIFEST {
            return Err("The explorer's bootstrap list is too large.".into());
        }
        parse_manifest(&body)
    }

    pub fn open(&self, offer: &Offer) -> Result<impl Read, String> {
        Ok(self.get(&offer.url)?.into_reader())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("hive-boot-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn sha(bytes: &[u8]) -> String {
        Sha256::digest(bytes)
            .iter()
            .map(|b| format!("{b:02x}"))
            .collect()
    }

    /// A tar.zst with the given entries: (path, Some(bytes)) for a file,
    /// (path, None) for a folder. Paths are written raw, so tests can use
    /// ones the tar crate's builder would refuse.
    fn archive(entries: &[(&str, Option<&[u8]>)]) -> Vec<u8> {
        let mut tar = tar::Builder::new(Vec::new());
        for (path, body) in entries {
            let mut header = tar::Header::new_gnu();
            let name = header.as_old_mut().name.as_mut();
            name[..path.len()].copy_from_slice(path.as_bytes());
            match body {
                Some(bytes) => {
                    header.set_entry_type(tar::EntryType::Regular);
                    header.set_size(bytes.len() as u64);
                    header.set_mode(0o600);
                    header.set_cksum();
                    tar.append(&header, *bytes).unwrap();
                }
                None => {
                    header.set_entry_type(tar::EntryType::Directory);
                    header.set_size(0);
                    header.set_mode(0o700);
                    header.set_cksum();
                    tar.append(&header, io::empty()).unwrap();
                }
            }
        }
        zstd::encode_all(&tar.into_inner().unwrap()[..], 3).unwrap()
    }

    fn offer_for(bytes: &[u8]) -> Offer {
        Offer {
            height: 10,
            size: bytes.len() as u64,
            sha256: sha(bytes),
            url: format!("{ORIGIN}/bootstraps/bootstrap-10.tar.zst"),
            block_hash: None,
            created: None,
        }
    }

    fn run(bytes: &[u8], offer: &Offer, dir: &Path) -> Result<Vec<Progress>, String> {
        let mut seen = Vec::new();
        install(
            &mut &bytes[..],
            offer,
            dir,
            &mut |p| seen.push(p),
            &AtomicBool::new(false),
        )
        .map(|()| seen)
    }

    #[test]
    fn the_manifest_keeps_only_well_formed_offers() {
        let h = "a".repeat(64);
        let body = format!(
            r#"{{"bootstraps": [
                {{"height": 850000, "size": 5, "sha256": "{h}", "url": "/bootstraps/bootstrap-850000.tar.zst"}},
                {{"height": 860000, "size": 7, "sha256": "{h}", "url": "/bootstraps/bootstrap-860000.tar.zst",
                  "block_hash": "{h}", "created_utc": "2026-10-17T03:15:02Z"}},
                {{"height": 1, "size": 7, "sha256": "{h}", "url": "https://elsewhere/bootstraps/bootstrap-1.tar.zst"}},
                {{"height": 2, "size": 7, "sha256": "{h}", "url": "/bootstraps/bootstrap-3.tar.zst"}},
                {{"height": 4, "size": 7, "sha256": "ABC", "url": "/bootstraps/bootstrap-4.tar.zst"}},
                {{"height": 5, "size": 0, "sha256": "{h}", "url": "/bootstraps/bootstrap-5.tar.zst"}},
                {{"height": 6, "sha256": "{h}", "url": "/bootstraps/bootstrap-6.tar.zst"}}
            ]}}"#
        );
        let offers = parse_manifest(body.as_bytes()).unwrap();
        assert_eq!(
            offers.iter().map(|o| o.height).collect::<Vec<_>>(),
            [860000, 850000]
        );
        assert_eq!(
            offers[0].url,
            "https://explorer.b3hive.io/bootstraps/bootstrap-860000.tar.zst"
        );
        assert_eq!(offers[0].created.as_deref(), Some("2026-10-17T03:15:02Z"));
        assert!(parse_manifest(b"not json").is_err());
        assert!(parse_manifest(b"{}").unwrap().is_empty());
    }

    #[test]
    fn only_paths_inside_the_chain_folders_are_safe() {
        for good in [
            "blocks",
            "blocks/index/CURRENT",
            "./chainstate/LOCK",
            "flowmesh/ab/x",
        ] {
            assert!(safe_relative(Path::new(good)).is_some(), "{good}");
        }
        for bad in [
            "",
            "wallets/wallet.dat",
            "b3coin.conf",
            "../blocks",
            "blocks/../../x",
            "/blocks/x",
            "blocks/./../x",
        ] {
            assert!(safe_relative(Path::new(bad)).is_none(), "{bad}");
        }
    }

    #[test]
    fn a_fresh_folder_gets_the_chain_folders_and_nothing_else() {
        let dir = temp("ok");
        fs::write(dir.join("b3coin.conf"), "keep").unwrap();
        fs::create_dir(dir.join("chainstate")).unwrap(); // empty counts as fresh
        let bytes = archive(&[
            ("blocks", None),
            ("blocks/blk00000.dat", Some(b"block data")),
            ("blocks/index/CURRENT", Some(b"MANIFEST-1")),
            ("chainstate/CURRENT", Some(b"MANIFEST-2")),
            ("indexes/txindex", None),
        ]);
        let seen = run(&bytes, &offer_for(&bytes), &dir).unwrap();
        assert_eq!(
            fs::read(dir.join("blocks/blk00000.dat")).unwrap(),
            b"block data"
        );
        assert_eq!(
            fs::read(dir.join("blocks/index/CURRENT")).unwrap(),
            b"MANIFEST-1"
        );
        assert_eq!(
            fs::read(dir.join("chainstate/CURRENT")).unwrap(),
            b"MANIFEST-2"
        );
        assert!(dir.join("indexes/txindex").is_dir());
        assert_eq!(fs::read(dir.join("b3coin.conf")).unwrap(), b"keep");
        // The download and staging are gone.
        let mut names: Vec<_> = fs::read_dir(&dir)
            .unwrap()
            .map(|e| e.unwrap().file_name().into_string().unwrap())
            .collect();
        names.sort();
        assert_eq!(names, ["b3coin.conf", "blocks", "chainstate", "indexes"]);
        assert!(matches!(seen[0], Progress::Downloading { received: 0, .. }));
        assert!(seen.contains(&Progress::Verifying));
        assert_eq!(seen.last(), Some(&Progress::Done { height: 10 }));
        assert!(!is_fresh(&dir));
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn a_folder_with_a_chain_is_never_overwritten() {
        let dir = temp("used");
        fs::create_dir_all(dir.join("blocks")).unwrap();
        fs::write(dir.join("blocks/blk00000.dat"), "mine").unwrap();
        let bytes = archive(&[("blocks/blk00000.dat", Some(b"theirs"))]);
        assert!(run(&bytes, &offer_for(&bytes), &dir).is_err());
        assert_eq!(fs::read(dir.join("blocks/blk00000.dat")).unwrap(), b"mine");
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn a_wrong_size_or_hash_installs_nothing() {
        let bytes = archive(&[("blocks/blk00000.dat", Some(b"block data"))]);
        let mut wrong_hash = offer_for(&bytes);
        wrong_hash.sha256 = sha(b"something else");
        let mut too_small = offer_for(&bytes);
        too_small.size -= 1;
        let mut too_large = offer_for(&bytes);
        too_large.size += 1;
        for (name, offer) in [
            ("hash", wrong_hash),
            ("small", too_small),
            ("large", too_large),
        ] {
            let dir = temp(name);
            let err = run(&bytes, &offer, &dir).unwrap_err();
            assert!(is_fresh(&dir), "{name}: {err}");
            assert_eq!(fs::read_dir(&dir).unwrap().count(), 0, "{name}: leftovers");
            let _ = fs::remove_dir_all(dir);
        }
    }

    #[test]
    fn an_archive_reaching_outside_the_chain_folders_installs_nothing() {
        for (name, entries) in [
            (
                "wallet",
                vec![
                    ("blocks/a", Some(&b"x"[..])),
                    ("wallets/wallet.dat", Some(&b"evil"[..])),
                ],
            ),
            (
                "conf",
                vec![("b3coin.conf", Some(&b"rpcallowip=0.0.0.0/0"[..]))],
            ),
            (
                "dotdot",
                vec![("blocks/../b3coin.conf", Some(&b"evil"[..]))],
            ),
        ] {
            let dir = temp(name);
            let bytes = archive(&entries);
            let err = run(&bytes, &offer_for(&bytes), &dir).unwrap_err();
            assert!(err.contains("Nothing was installed"), "{name}: {err}");
            assert_eq!(fs::read_dir(&dir).unwrap().count(), 0, "{name}: leftovers");
            let _ = fs::remove_dir_all(dir);
        }
    }

    #[test]
    fn links_are_refused() {
        let dir = temp("link");
        let mut tar = tar::Builder::new(Vec::new());
        let mut header = tar::Header::new_gnu();
        header.set_entry_type(tar::EntryType::Symlink);
        header.set_size(0);
        tar.append_link(&mut header, "blocks/index", "/etc")
            .unwrap();
        let bytes = zstd::encode_all(&tar.into_inner().unwrap()[..], 3).unwrap();
        let err = run(&bytes, &offer_for(&bytes), &dir).unwrap_err();
        assert!(err.contains("does not unpack"), "{err}");
        assert_eq!(fs::read_dir(&dir).unwrap().count(), 0);
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn a_cancel_stops_and_cleans_up() {
        let dir = temp("cancel");
        let bytes = archive(&[("blocks/blk00000.dat", Some(b"block data"))]);
        let cancel = AtomicBool::new(true);
        let err = install(
            &mut &bytes[..],
            &offer_for(&bytes),
            &dir,
            &mut |_| {},
            &cancel,
        )
        .unwrap_err();
        assert_eq!(err, CANCELLED);
        assert_eq!(fs::read_dir(&dir).unwrap().count(), 0);
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn only_the_explorer_is_asked() {
        let explorer = Explorer::new();
        let mut offer = offer_for(b"x");
        offer.url = "https://example.com/bootstraps/bootstrap-10.tar.zst".into();
        assert!(explorer.open(&offer).is_err());
    }
}
