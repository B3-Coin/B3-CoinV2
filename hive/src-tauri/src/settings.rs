//! The node options B3-Qt's Options dialog offers, kept where Qt keeps
//! them: b3coind's own read-write settings file, settings.json in the chain
//! folder. b3coind reads it at start, above b3coin.conf and below the
//! command line, so a change takes a restart. Both wallets share the file,
//! and keys B3 Hive does not manage are kept as they are.

use std::fs;
use std::net::SocketAddr;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};
use serde_json::{Map, Value};

pub const FILE: &str = "settings.json";

/// Qt's limits: the smallest cache b3coind accepts, and its most script
/// verification threads. Negative thread counts leave that many cores free.
pub const MIN_DBCACHE_MIB: u64 = 4;
pub const MAX_DBCACHE_MIB: u64 = 1024 * 1024;
pub const MAX_SCRIPT_THREADS: i64 = 15;
pub const MIN_SCRIPT_THREADS: i64 = -64;

/// None leaves the option to b3coin.conf or b3coind's default.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct NodeSettings {
    /// -dbcache, in MiB.
    pub dbcache_mib: Option<u64>,
    /// -par: 0 picks a count from the cores.
    pub script_threads: Option<i64>,
    /// -listen: accept incoming connections.
    pub listen: Option<bool>,
    /// -natpmp: map the listening port with PCP or NAT-PMP.
    pub natpmp: Option<bool>,
    /// -proxy, as ip:port.
    pub proxy: Option<String>,
    /// -onion, a separate SOCKS5 proxy for Tor onion services, as ip:port.
    pub onion: Option<String>,
    /// -port: the P2P listening port, for a second B3 node on one computer.
    pub port: Option<u16>,
}

const KEYS: [&str; 7] = [
    "dbcache", "par", "listen", "natpmp", "proxy", "onion", "port",
];

pub fn path(chain_dir: &Path) -> PathBuf {
    chain_dir.join(FILE)
}

/// The file's object, or an empty one when there is no file yet. A file
/// b3coind could not read either is reported, never replaced.
fn read_object(chain_dir: &Path) -> Result<Map<String, Value>, String> {
    let file = path(chain_dir);
    let text = match fs::read_to_string(&file) {
        Ok(text) => text,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(Map::new()),
        Err(e) => return Err(format!("Cannot read {}: {e}", file.display())),
    };
    match serde_json::from_str(&text) {
        Ok(Value::Object(map)) => Ok(map),
        _ => Err(format!(
            "{} is not a settings file b3coind can read, so B3 Hive leaves it alone.",
            file.display()
        )),
    }
}

fn unreadable(key: &str, value: &Value) -> String {
    format!("{FILE} sets {key} to {value}, which B3 Hive cannot read, so it leaves the file alone.")
}

/// Qt writes numbers as strings, for Bitcoin 22.x; both forms are read.
fn integer(key: &str, value: &Value) -> Result<Option<i64>, String> {
    match value {
        Value::Null => Ok(None),
        Value::Number(n) => n.as_i64().map(Some).ok_or_else(|| unreadable(key, value)),
        Value::String(s) => s
            .trim()
            .parse()
            .map(Some)
            .map_err(|_| unreadable(key, value)),
        _ => Err(unreadable(key, value)),
    }
}

/// b3coind's reading of a setting as a flag (InterpretBool).
fn flag(key: &str, value: &Value) -> Result<Option<bool>, String> {
    match value {
        Value::Null => Ok(None),
        Value::Bool(b) => Ok(Some(*b)),
        Value::Number(n) => Ok(Some(n.as_i64().ok_or_else(|| unreadable(key, value))? != 0)),
        Value::String(s) if s.is_empty() => Ok(Some(true)),
        Value::String(s) => s
            .trim()
            .parse::<i64>()
            .map(|n| Some(n != 0))
            .map_err(|_| unreadable(key, value)),
        _ => Err(unreadable(key, value)),
    }
}

fn text(key: &str, value: &Value) -> Result<Option<String>, String> {
    match value {
        Value::Null => Ok(None),
        Value::String(s) if s.is_empty() => Ok(None),
        Value::String(s) => Ok(Some(s.clone())),
        _ => Err(unreadable(key, value)),
    }
}

pub fn read(chain_dir: &Path) -> Result<NodeSettings, String> {
    let map = read_object(chain_dir)?;
    let get = |key: &str| map.get(key).unwrap_or(&Value::Null);
    let dbcache = integer("dbcache", get("dbcache"))?;
    Ok(NodeSettings {
        dbcache_mib: dbcache
            .map(|n| u64::try_from(n).map_err(|_| unreadable("dbcache", get("dbcache"))))
            .transpose()?,
        script_threads: integer("par", get("par"))?,
        listen: flag("listen", get("listen"))?,
        natpmp: flag("natpmp", get("natpmp"))?,
        proxy: text("proxy", get("proxy"))?,
        onion: text("onion", get("onion"))?,
        port: integer("port", get("port"))?
            .map(|n| {
                u16::try_from(n)
                    .ok()
                    .filter(|p| *p != 0)
                    .ok_or_else(|| unreadable("port", get("port")))
            })
            .transpose()?,
    })
}

/// A proxy as b3coind takes it: an IP address and a port.
fn proxy_address(what: &str, value: &str) -> Result<String, String> {
    match value.trim().parse::<SocketAddr>() {
        Ok(addr) if addr.port() != 0 => Ok(addr.to_string()),
        _ => Err(format!(
            "The {what} must be an IP address and a port, such as 127.0.0.1:9050."
        )),
    }
}

impl NodeSettings {
    /// The settings with proxies in b3coind's form, or why they cannot be used.
    pub fn validated(&self) -> Result<NodeSettings, String> {
        if let Some(mib) = self.dbcache_mib {
            if !(MIN_DBCACHE_MIB..=MAX_DBCACHE_MIB).contains(&mib) {
                return Err(format!(
                    "The database cache must be between {MIN_DBCACHE_MIB} and {MAX_DBCACHE_MIB} MiB."
                ));
            }
        }
        if let Some(n) = self.script_threads {
            if !(MIN_SCRIPT_THREADS..=MAX_SCRIPT_THREADS).contains(&n) {
                return Err(format!(
                    "Script verification threads must be between {MIN_SCRIPT_THREADS} and {MAX_SCRIPT_THREADS}."
                ));
            }
        }
        if self.port == Some(0) {
            return Err("The listening port must be between 1 and 65535.".into());
        }
        Ok(NodeSettings {
            proxy: self
                .proxy
                .as_deref()
                .map(|p| proxy_address("proxy", p))
                .transpose()?,
            onion: self
                .onion
                .as_deref()
                .map(|p| proxy_address("Tor proxy", p))
                .transpose()?,
            ..self.clone()
        })
    }
}

/// Fails, touching nothing, when the file exists but cannot be updated
/// safely. Checked before the node is stopped for a change.
pub fn check(chain_dir: &Path) -> Result<(), String> {
    read(chain_dir).map(|_| ())
}

/// Writes `settings` into the file, keeping every other key. The node must
/// not be running: b3coind may rewrite the file while it runs.
pub fn write(chain_dir: &Path, settings: &NodeSettings) -> Result<(), String> {
    let settings = settings.validated()?;
    // A value B3 Hive cannot read may be one Qt or b3coind relies on.
    check(chain_dir)?;
    let mut map = read_object(chain_dir)?;
    for key in KEYS {
        map.remove(key);
    }
    // Numbers as strings, as Qt writes them.
    let mut put = |key: &str, value: Option<Value>| {
        if let Some(value) = value {
            map.insert(key.into(), value);
        }
    };
    put(
        "dbcache",
        settings.dbcache_mib.map(|n| Value::String(n.to_string())),
    );
    put(
        "par",
        settings
            .script_threads
            .map(|n| Value::String(n.to_string())),
    );
    put("listen", settings.listen.map(Value::Bool));
    put("natpmp", settings.natpmp.map(Value::Bool));
    put("proxy", settings.proxy.map(Value::String));
    put("onion", settings.onion.map(Value::String));
    put("port", settings.port.map(|n| Value::String(n.to_string())));

    fs::create_dir_all(chain_dir)
        .map_err(|e| format!("Cannot create {}: {e}", chain_dir.display()))?;
    let file = path(chain_dir);
    let temp = chain_dir.join(format!("{FILE}.hive-tmp"));
    let body = serde_json::to_string_pretty(&Value::Object(map)).expect("a JSON object") + "\n";
    fs::write(&temp, body).map_err(|e| format!("Cannot write {}: {e}", temp.display()))?;
    fs::rename(&temp, &file).map_err(|e| {
        let _ = fs::remove_file(&temp);
        format!("Cannot write {}: {e}", file.display())
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("hive-settings-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn no_file_means_all_defaults() {
        let dir = temp("none");
        assert_eq!(read(&dir).unwrap(), NodeSettings::default());
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn reads_what_qt_writes() {
        let dir = temp("qt");
        fs::write(
            path(&dir),
            r#"{"_warning_": "generated", "dbcache": "600", "par": "-2", "listen": false,
                "natpmp": "1", "proxy": "127.0.0.1:9050", "onion": "", "port": "5649", "wallet": ["main"]}"#,
        )
        .unwrap();
        assert_eq!(
            read(&dir).unwrap(),
            NodeSettings {
                dbcache_mib: Some(600),
                script_threads: Some(-2),
                listen: Some(false),
                natpmp: Some(true),
                proxy: Some("127.0.0.1:9050".into()),
                onion: None,
                port: Some(5649),
            }
        );
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn writing_keeps_other_keys_and_round_trips() {
        let dir = temp("write");
        fs::write(
            path(&dir),
            r#"{"_warning_": "generated", "wallet": ["main"], "proxy": "10.0.0.1:1080", "lang": "de"}"#,
        )
        .unwrap();
        let wanted = NodeSettings {
            dbcache_mib: Some(1024),
            script_threads: Some(0),
            listen: Some(true),
            natpmp: Some(false),
            proxy: None,
            onion: Some(" [::1]:9050 ".into()),
            port: Some(5649),
        };
        write(&dir, &wanted).unwrap();
        let map = read_object(&dir).unwrap();
        assert_eq!(map["wallet"], serde_json::json!(["main"]));
        assert_eq!(map["lang"], "de");
        assert_eq!(map["_warning_"], "generated");
        assert_eq!(map["dbcache"], "1024");
        assert!(!map.contains_key("proxy"));
        assert_eq!(map["onion"], "[::1]:9050");
        assert_eq!(map["port"], "5649");
        assert_eq!(
            read(&dir).unwrap(),
            NodeSettings {
                onion: Some("[::1]:9050".into()),
                ..wanted
            }
        );
        assert!(!dir.join("settings.json.hive-tmp").exists());

        // Defaults remove every managed key, and only those.
        write(&dir, &NodeSettings::default()).unwrap();
        let map = read_object(&dir).unwrap();
        let mut keys: Vec<_> = map.keys().cloned().collect();
        keys.sort();
        assert_eq!(keys, ["_warning_", "lang", "wallet"]);
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn a_file_it_cannot_read_is_left_alone() {
        for (name, body) in [
            ("broken", "{not json"),
            ("array", "[]"),
            ("dbcache", r#"{"dbcache": "lots"}"#),
            ("negative", r#"{"dbcache": -5}"#),
            ("proxy", r#"{"proxy": 9050}"#),
        ] {
            let dir = temp(name);
            fs::write(path(&dir), body).unwrap();
            assert!(check(&dir).is_err(), "{name}");
            assert!(write(&dir, &NodeSettings::default()).is_err(), "{name}");
            assert_eq!(fs::read_to_string(path(&dir)).unwrap(), body, "{name}");
            let _ = fs::remove_dir_all(dir);
        }
    }

    #[test]
    fn out_of_range_values_are_refused() {
        let bad = [
            NodeSettings {
                dbcache_mib: Some(3),
                ..Default::default()
            },
            NodeSettings {
                script_threads: Some(16),
                ..Default::default()
            },
            NodeSettings {
                proxy: Some("localhost:9050".into()),
                ..Default::default()
            },
            NodeSettings {
                port: Some(0),
                ..Default::default()
            },
            NodeSettings {
                onion: Some("127.0.0.1".into()),
                ..Default::default()
            },
            NodeSettings {
                proxy: Some("127.0.0.1:0".into()),
                ..Default::default()
            },
        ];
        for settings in bad {
            assert!(settings.validated().is_err(), "{settings:?}");
        }
    }
}
