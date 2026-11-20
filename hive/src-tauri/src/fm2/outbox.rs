//! The client's durable outbox and its key file.
//!
//! Every action is signed ONCE, and its exact bytes are on disk here BEFORE
//! they leave the client. A crash between signing and sending, or between
//! sending and the reply, loses nothing: the same bytes are posted again and
//! a seat that already holds the action answers DUPLICATE. The outbox never
//! signs a second time, and the highest nonce it ever signed is the floor
//! for the next one (src/flowmesh2/cli/outbox.h has the same contract; the
//! file format here is B3 Hive's own, so an outbox is never shared with
//! fm2-cli, and neither is its key).
//!
//! File `outbox.log`: the magic `B3HIVEOB1\n`, then records. A record is
//! `u32 length | payload | 8 bytes of SHA-256(length || payload)`, appended
//! and flushed to disk before the call returns. A torn last record (the
//! process died while writing: the record is short, or the file ends in
//! zeros) is cut off when the file is opened; any other damaged record
//! refuses the file, because cutting it could drop a signed action.
//!
//! Records:
//! - ACTION (1): label, unix time, the exact signed bytes.
//! - RESULT (2): label, what a certified reply proved (INCLUDED with the
//!   slot, the outcome index and the outcome's bytes, or EXPIRED with the
//!   proving head). Written only after the proof was checked.
//! - FILL (3): a fill of one of this account's resting orders as a seat's
//!   trade stream listed it. A claim, kept so the open orders can be shown
//!   after a restart.

use std::fs::{File, OpenOptions};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};

use sha2::{Digest, Sha256};

use super::bytes::{hex, put_compact, unhex, Reader, H256};
use super::crypto::{wipe, SecretKey};
use super::types::{Action, Outcome, Profile};
use super::Error;

const MAGIC: &[u8] = b"B3HIVEOB1\n";
const MAX_RECORD_BYTES: usize = 256 * 1024;
const MAX_LABEL_BYTES: usize = 64;

const RECORD_ACTION: u8 = 1;
const RECORD_RESULT: u8 = 2;
const RECORD_FILL: u8 = 3;

/// What a certified reply proved about an action.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Certified {
    /// Included in the slot at `height` as outcome `index`.
    Included {
        height: u64,
        index: u32,
        outcome: Outcome,
    },
    /// Provably never includable; `height` is the proving head.
    Expired { height: u64 },
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Entry {
    pub label: String,
    /// Unix seconds when the action was signed.
    pub created: u64,
    /// The exact signed action.
    pub bytes: Vec<u8>,
    pub action: Action,
    pub result: Option<Certified>,
}

/// A fill of an own resting order, as a seat's trade stream listed it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SeenFill {
    pub order: H256,
    pub taker: H256,
    pub slot: u64,
    pub price_ticks: u64,
    pub lots: u64,
}

pub struct Outbox {
    file: File,
    profile: Profile,
    entries: Vec<Entry>,
    fills: Vec<SeenFill>,
    /// Bytes of a torn last record that were cut off at open.
    pub recovered_bytes: u64,
}

fn storage(what: &str, error: impl std::fmt::Display) -> Error {
    Error::Storage(format!("outbox: {what}: {error}"))
}

fn put_text(out: &mut Vec<u8>, text: &str) {
    put_compact(out, text.len() as u64);
    out.extend_from_slice(text.as_bytes());
}

fn take_text(reader: &mut Reader<'_>) -> Result<String, Error> {
    let length = reader.count(MAX_LABEL_BYTES)?;
    String::from_utf8(reader.take(length)?.to_vec()).map_err(|_| Error::Decode("outbox label"))
}

fn checksum(length: &[u8; 4], payload: &[u8]) -> [u8; 8] {
    let mut hasher = Sha256::new();
    hasher.update(length);
    hasher.update(payload);
    hasher.finalize()[..8].try_into().expect("8 bytes")
}

impl Outbox {
    /// Opens `dir/outbox.log`, creating the directory and the file when
    /// they are missing.
    pub fn open(dir: &Path, profile: &Profile) -> Result<Self, Error> {
        std::fs::create_dir_all(dir).map_err(|e| storage("cannot create its folder", e))?;
        let path = dir.join("outbox.log");
        let mut file = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .open(&path)
            .map_err(|e| storage("cannot open", e))?;
        let mut raw = Vec::new();
        file.read_to_end(&mut raw)
            .map_err(|e| storage("cannot read", e))?;
        let mut outbox = Self {
            file,
            profile: profile.clone(),
            entries: Vec::new(),
            fills: Vec::new(),
            recovered_bytes: 0,
        };
        if raw.is_empty() {
            outbox
                .file
                .write_all(MAGIC)
                .map_err(|e| storage("cannot write", e))?;
            outbox
                .file
                .sync_all()
                .map_err(|e| storage("cannot flush", e))?;
            return Ok(outbox);
        }
        if !raw.starts_with(MAGIC) {
            return Err(Error::Storage(
                "outbox: the file is not a B3 Hive outbox".into(),
            ));
        }
        let mut position = MAGIC.len();
        while position < raw.len() {
            let Some(payload) = Self::record_at(&raw[position..]) else {
                // Only a torn tail may be cut: whatever follows a damaged
                // record that is not the last would be lost silently.
                let rest = &raw[position..];
                let claimed = rest
                    .get(..4)
                    .map(|l| u32::from_le_bytes(l.try_into().expect("4 bytes")) as usize);
                // A crash can also leave the file extended but never filled.
                let torn = match claimed {
                    None => true,
                    Some(length) => length <= MAX_RECORD_BYTES && rest.len() < 4 + length + 8,
                } || rest.iter().all(|byte| *byte == 0);
                if !torn {
                    return Err(Error::Storage(
                        "outbox: a record is damaged; the file was left as it is".into(),
                    ));
                }
                outbox.recovered_bytes = rest.len() as u64;
                outbox
                    .file
                    .set_len(position as u64)
                    .map_err(|e| storage("cannot cut a torn record", e))?;
                outbox
                    .file
                    .sync_all()
                    .map_err(|e| storage("cannot flush", e))?;
                break;
            };
            outbox.apply(payload)?;
            position += 4 + payload.len() + 8;
        }
        outbox
            .file
            .seek(SeekFrom::End(0))
            .map_err(|e| storage("cannot seek", e))?;
        Ok(outbox)
    }

    /// The payload of the complete, intact record at the start of `bytes`.
    fn record_at(bytes: &[u8]) -> Option<&[u8]> {
        let length_bytes: [u8; 4] = bytes.get(..4)?.try_into().ok()?;
        let length = u32::from_le_bytes(length_bytes) as usize;
        if length > MAX_RECORD_BYTES {
            return None;
        }
        let payload = bytes.get(4..4 + length)?;
        let sum = bytes.get(4 + length..4 + length + 8)?;
        (sum == checksum(&length_bytes, payload)).then_some(payload)
    }

    fn apply(&mut self, payload: &[u8]) -> Result<(), Error> {
        let mut reader = Reader::new(payload);
        match reader.u8()? {
            RECORD_ACTION => {
                let label = take_text(&mut reader)?;
                let created = reader.u64()?;
                let length = reader.count(MAX_RECORD_BYTES)?;
                let bytes = reader.take(length)?.to_vec();
                reader.finish()?;
                let action = Action::decode(&bytes, &self.profile)?;
                self.entries.push(Entry {
                    label,
                    created,
                    bytes,
                    action,
                    result: None,
                });
            }
            RECORD_RESULT => {
                let label = take_text(&mut reader)?;
                let kind = reader.u8()?;
                let height = reader.u64()?;
                let index = reader.u32()?;
                let length = reader.count(MAX_RECORD_BYTES)?;
                let outcome = reader.take(length)?;
                reader.finish()?;
                let result = match kind {
                    1 => Certified::Included {
                        height,
                        index,
                        outcome: Outcome::decode(outcome, &self.profile)?,
                    },
                    2 => Certified::Expired { height },
                    _ => return Err(Error::Decode("outbox result kind")),
                };
                let entry = self
                    .entries
                    .iter_mut()
                    .find(|entry| entry.label == label)
                    .ok_or(Error::Decode("an outbox result names no action"))?;
                entry.result = Some(result);
            }
            RECORD_FILL => {
                let fill = SeenFill {
                    order: reader.h256()?,
                    taker: reader.h256()?,
                    slot: reader.u64()?,
                    price_ticks: reader.u64()?,
                    lots: reader.u64()?,
                };
                reader.finish()?;
                self.fills.push(fill);
            }
            _ => return Err(Error::Decode("unknown outbox record")),
        }
        Ok(())
    }

    /// Appends one record and flushes it to disk before returning.
    fn append(&mut self, payload: &[u8]) -> Result<(), Error> {
        if payload.len() > MAX_RECORD_BYTES {
            return Err(Error::Oversize("outbox record"));
        }
        let length = (payload.len() as u32).to_le_bytes();
        let mut record = Vec::with_capacity(4 + payload.len() + 8);
        record.extend_from_slice(&length);
        record.extend_from_slice(payload);
        record.extend_from_slice(&checksum(&length, payload));
        self.file
            .write_all(&record)
            .map_err(|e| storage("cannot write", e))?;
        self.file
            .sync_data()
            .map_err(|e| storage("cannot flush", e))
    }

    /// Makes `bytes` durable under `label` before returning. The label must
    /// be new and the bytes must decode as an action.
    pub fn add(&mut self, label: &str, created: u64, bytes: &[u8]) -> Result<(), Error> {
        if label.is_empty() || label.len() > MAX_LABEL_BYTES {
            return Err(Error::Invalid("an outbox label has 1 to 64 bytes".into()));
        }
        if self.entries.iter().any(|entry| entry.label == label) {
            return Err(Error::Invalid("the outbox already holds this label".into()));
        }
        let action = Action::decode(bytes, &self.profile)?;
        let mut payload = vec![RECORD_ACTION];
        put_text(&mut payload, label);
        payload.extend_from_slice(&created.to_le_bytes());
        put_compact(&mut payload, bytes.len() as u64);
        payload.extend_from_slice(bytes);
        self.append(&payload)?;
        self.entries.push(Entry {
            label: label.to_owned(),
            created,
            bytes: bytes.to_vec(),
            action,
            result: None,
        });
        Ok(())
    }

    /// Records what a verified reply proved about `label`, durable before
    /// returning. Once per label: a certified end is final.
    pub fn record(&mut self, label: &str, result: Certified) -> Result<(), Error> {
        let position = self
            .entries
            .iter()
            .position(|entry| entry.label == label)
            .ok_or(Error::Invalid("no outbox entry with this label".into()))?;
        if self.entries[position].result.is_some() {
            return Err(Error::Invalid(
                "the outbox entry already has a result".into(),
            ));
        }
        let mut payload = vec![RECORD_RESULT];
        put_text(&mut payload, label);
        match &result {
            Certified::Included {
                height,
                index,
                outcome,
            } => {
                payload.push(1);
                payload.extend_from_slice(&height.to_le_bytes());
                payload.extend_from_slice(&index.to_le_bytes());
                put_compact(&mut payload, outcome.bytes().len() as u64);
                payload.extend_from_slice(outcome.bytes());
            }
            Certified::Expired { height } => {
                payload.push(2);
                payload.extend_from_slice(&height.to_le_bytes());
                payload.extend_from_slice(&0u32.to_le_bytes());
                put_compact(&mut payload, 0);
            }
        }
        self.append(&payload)?;
        self.entries[position].result = Some(result);
        Ok(())
    }

    /// Notes a fill a seat's trade stream listed for an own order; true when
    /// it was new. One fill is one (order, taker, slot).
    pub fn note_fill(&mut self, fill: SeenFill) -> Result<bool, Error> {
        if self
            .fills
            .iter()
            .any(|f| f.order == fill.order && f.taker == fill.taker && f.slot == fill.slot)
        {
            return Ok(false);
        }
        let mut payload = vec![RECORD_FILL];
        payload.extend_from_slice(&fill.order);
        payload.extend_from_slice(&fill.taker);
        payload.extend_from_slice(&fill.slot.to_le_bytes());
        payload.extend_from_slice(&fill.price_ticks.to_le_bytes());
        payload.extend_from_slice(&fill.lots.to_le_bytes());
        self.append(&payload)?;
        self.fills.push(fill);
        Ok(true)
    }

    pub fn entries(&self) -> &[Entry] {
        &self.entries
    }

    pub fn fills(&self) -> &[SeenFill] {
        &self.fills
    }

    pub fn find(&self, label: &str) -> Option<&Entry> {
        self.entries.iter().find(|entry| entry.label == label)
    }

    /// The highest nonce this outbox ever signed for `account`.
    pub fn highest_nonce(&self, account: &H256) -> Option<u64> {
        self.entries
            .iter()
            .filter(|e| e.action.account() == *account)
            .map(|e| e.action.nonce)
            .max()
    }
}

// ----------------------------------------------------------------- key file

/// The key file of an account folder: 64 hex digits and a newline, the form
/// fm2-cli writes. It is not encrypted; FlowMesh V2 is regtest only.
pub fn key_path(dir: &Path) -> PathBuf {
    dir.join("key")
}

/// Loads the key of `dir`, or `None` when the folder has none yet.
pub fn load_key(dir: &Path) -> Result<Option<SecretKey>, Error> {
    let path = key_path(dir);
    let mut text = match std::fs::read(&path) {
        Ok(text) => text,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(e) => return Err(Error::Storage(format!("cannot read the key file: {e}"))),
    };
    let secret = std::str::from_utf8(&text)
        .ok()
        .map(str::trim_end)
        .and_then(unhex);
    wipe(&mut text);
    let invalid = Error::Storage("the key file does not hold a valid key".into());
    let mut secret: [u8; 32] = secret
        .and_then(|bytes| bytes.try_into().ok())
        .ok_or(invalid.clone())?;
    let key = SecretKey::from_bytes(&secret);
    wipe(&mut secret);
    key.map(Some).ok_or(invalid)
}

/// Creates a new key in `dir`. It refuses to replace an existing key file,
/// and the file is on disk before the key is returned.
pub fn create_key(dir: &Path) -> Result<SecretKey, Error> {
    std::fs::create_dir_all(dir)
        .map_err(|e| Error::Storage(format!("cannot create the account folder: {e}")))?;
    let (mut secret, key) = SecretKey::generate();
    let mut line = hex(&secret).into_bytes();
    line.push(b'\n');
    wipe(&mut secret);
    let mut options = OpenOptions::new();
    options.write(true).create_new(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.mode(0o600);
    }
    let written = options
        .open(key_path(dir))
        .and_then(|mut file| file.write_all(&line).and_then(|()| file.sync_all()));
    wipe(&mut line);
    written.map_err(|e| Error::Storage(format!("cannot write the key file: {e}")))?;
    Ok(key)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::fm2::types::{Payload, SIDE_BUY, TIF_GTC};

    fn temp(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("b3hive-outbox-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        dir
    }

    fn signed(key: &SecretKey, nonce: u64) -> Vec<u8> {
        let payload = Payload::Place {
            market: [9; 32],
            side: SIDE_BUY,
            tif: TIF_GTC,
            price_ticks: 100,
            lots: 5,
        };
        let mut action = Action {
            domain: [1; 32],
            signer: key.public(),
            nonce,
            ref_slot: 40,
            max_delay: 64,
            kind: payload.kind(),
            payload: payload.encode().unwrap(),
            sig: [0; 64],
        };
        action.sig = key.sign(&action.id());
        action.encode()
    }

    fn outcome() -> Outcome {
        let profile = Profile::dev_regtest();
        let text: serde_json::Value =
            serde_json::from_str(include_str!("../../tests/fixtures/fm2/status_filled.json"))
                .unwrap();
        let encoded = unhex(text["included"]["outcome"]["encoded"].as_str().unwrap()).unwrap();
        Outcome::decode(&encoded, &profile).unwrap()
    }

    #[test]
    fn what_was_added_is_there_after_reopening() {
        let dir = temp("reopen");
        let profile = Profile::dev_regtest();
        let (_, key) = SecretKey::generate();
        let account = crate::fm2::types::account_of(&key.public());
        let first = signed(&key, 1);
        let second = signed(&key, 2);
        {
            let mut outbox = Outbox::open(&dir, &profile).unwrap();
            assert_eq!(outbox.highest_nonce(&account), None);
            outbox.add("PLACE-1", 1_700_000_000, &first).unwrap();
            outbox.add("PLACE-2", 1_700_000_001, &second).unwrap();
            outbox
                .record(
                    "PLACE-1",
                    Certified::Included {
                        height: 408,
                        index: 0,
                        outcome: outcome(),
                    },
                )
                .unwrap();
            outbox
                .record("PLACE-2", Certified::Expired { height: 500 })
                .unwrap();
            let fill = SeenFill {
                order: [5; 32],
                taker: [6; 32],
                slot: 9,
                price_ticks: 100,
                lots: 2,
            };
            assert_eq!(outbox.note_fill(fill.clone()), Ok(true));
            assert_eq!(outbox.note_fill(fill), Ok(false));
        }
        let outbox = Outbox::open(&dir, &profile).unwrap();
        assert_eq!(outbox.recovered_bytes, 0);
        assert_eq!(outbox.entries().len(), 2);
        assert_eq!(outbox.entries()[0].bytes, first);
        assert_eq!(outbox.entries()[0].created, 1_700_000_000);
        assert_eq!(
            outbox.entries()[0].result,
            Some(Certified::Included {
                height: 408,
                index: 0,
                outcome: outcome()
            })
        );
        assert_eq!(
            outbox.find("PLACE-2").unwrap().result,
            Some(Certified::Expired { height: 500 })
        );
        assert_eq!(outbox.fills().len(), 1);
        assert_eq!(outbox.highest_nonce(&account), Some(2));
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn a_label_is_used_once_and_a_result_is_final() {
        let dir = temp("once");
        let profile = Profile::dev_regtest();
        let (_, key) = SecretKey::generate();
        let mut outbox = Outbox::open(&dir, &profile).unwrap();
        outbox.add("a", 0, &signed(&key, 1)).unwrap();
        assert!(matches!(
            outbox.add("a", 0, &signed(&key, 2)),
            Err(Error::Invalid(_))
        ));
        assert!(matches!(
            outbox.add("", 0, &signed(&key, 2)),
            Err(Error::Invalid(_))
        ));
        assert!(
            outbox.add("b", 0, &[1, 2, 3]).is_err(),
            "bytes that are not an action"
        );
        assert!(matches!(
            outbox.record("nope", Certified::Expired { height: 1 }),
            Err(Error::Invalid(_))
        ));
        outbox
            .record("a", Certified::Expired { height: 1 })
            .unwrap();
        assert!(matches!(
            outbox.record("a", Certified::Expired { height: 2 }),
            Err(Error::Invalid(_))
        ));
        drop(outbox);
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn a_torn_last_record_is_cut_and_the_rest_kept() {
        let dir = temp("torn");
        let profile = Profile::dev_regtest();
        let (_, key) = SecretKey::generate();
        {
            let mut outbox = Outbox::open(&dir, &profile).unwrap();
            outbox.add("a", 0, &signed(&key, 1)).unwrap();
            outbox.add("b", 0, &signed(&key, 2)).unwrap();
        }
        let path = dir.join("outbox.log");
        let full = std::fs::read(&path).unwrap();
        // The process died while the second record was being written.
        std::fs::write(&path, &full[..full.len() - 5]).unwrap();
        {
            let mut outbox = Outbox::open(&dir, &profile).unwrap();
            assert_eq!(outbox.entries().len(), 1);
            assert!(outbox.recovered_bytes > 0);
            // The file continues cleanly after the cut.
            outbox.add("c", 0, &signed(&key, 3)).unwrap();
        }
        let outbox = Outbox::open(&dir, &profile).unwrap();
        let labels: Vec<&str> = outbox.entries().iter().map(|e| e.label.as_str()).collect();
        assert_eq!(labels, ["a", "c"]);
        assert_eq!(outbox.recovered_bytes, 0);
        drop(outbox);
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn a_tail_of_zeros_is_cut_like_a_torn_record() {
        let dir = temp("zeros");
        let profile = Profile::dev_regtest();
        let (_, key) = SecretKey::generate();
        {
            let mut outbox = Outbox::open(&dir, &profile).unwrap();
            outbox.add("a", 0, &signed(&key, 1)).unwrap();
        }
        let path = dir.join("outbox.log");
        let mut bytes = std::fs::read(&path).unwrap();
        bytes.extend_from_slice(&[0u8; 300]);
        std::fs::write(&path, &bytes).unwrap();
        let outbox = Outbox::open(&dir, &profile).unwrap();
        assert_eq!((outbox.entries().len(), outbox.recovered_bytes), (1, 300));
        drop(outbox);
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn a_damaged_record_before_the_end_refuses_the_file() {
        let dir = temp("damaged");
        let profile = Profile::dev_regtest();
        let (_, key) = SecretKey::generate();
        {
            let mut outbox = Outbox::open(&dir, &profile).unwrap();
            outbox.add("a", 0, &signed(&key, 1)).unwrap();
            outbox.add("b", 0, &signed(&key, 2)).unwrap();
        }
        let path = dir.join("outbox.log");
        let mut bytes = std::fs::read(&path).unwrap();
        bytes[MAGIC.len() + 20] ^= 1;
        std::fs::write(&path, &bytes).unwrap();
        assert!(matches!(
            Outbox::open(&dir, &profile),
            Err(Error::Storage(_))
        ));
        // Nothing was cut: the file is as the damage left it.
        assert_eq!(std::fs::read(&path).unwrap(), bytes);
        std::fs::write(&path, b"something else").unwrap();
        assert!(matches!(
            Outbox::open(&dir, &profile),
            Err(Error::Storage(_))
        ));
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn a_key_is_created_once_and_loads_again() {
        let dir = temp("key");
        assert!(load_key(&dir).unwrap().is_none());
        let created = create_key(&dir).unwrap();
        assert!(
            create_key(&dir).is_err(),
            "an existing key is never replaced"
        );
        let loaded = load_key(&dir).unwrap().unwrap();
        assert_eq!(created.public(), loaded.public());
        let text = std::fs::read_to_string(key_path(&dir)).unwrap();
        assert_eq!(text.len(), 65);
        std::fs::write(key_path(&dir), "not a key\n").unwrap();
        assert!(load_key(&dir).is_err());
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
