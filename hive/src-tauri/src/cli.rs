//! B3 Hive's command line, for people used to starting B3-Qt with node
//! options: `b3-hive -datadir=D:\b3 -port=5649 -listen=0`. Every `-option`
//! or `--option` is handed to b3coind as it is, except the few B3 Hive sets
//! itself (its RPC login, the data folder, the network), which are refused
//! with a message rather than silently overridden.

use std::ffi::OsString;
use std::path::PathBuf;

#[derive(Debug, Default, PartialEq, Eq)]
pub struct CommandLine {
    /// `-datadir=<path>` / `--datadir <path>`: the data folder for this start.
    pub datadir: Option<PathBuf>,
    /// `-regtest`: the development network, as B3_HIVE_NETWORK=regtest.
    pub regtest: bool,
    /// Options handed to b3coind, in order, as `-name` or `-name=value`.
    pub forwarded: Vec<String>,
    /// Options B3 Hive owns, named without their value, for the error.
    pub refused: Vec<String>,
}

/// Options B3 Hive passes itself; a second value would be ignored or
/// would break the windows' login.
const OWNED: &[&str] = &[
    "server",
    "rpcbind",
    "rpcallowip",
    "rpcport",
    "rpcauth",
    "rpcuser",
    "rpcpassword",
    "rpcwhitelist",
    "rpcwhitelistdefault",
    "printtoconsole",
    "daemon",
    "daemonwait",
    "testnet",
    "testnet4",
    "signet",
    "chain",
    "fallbackfee",
    "maxtxfee",
];

pub fn parse(args: impl IntoIterator<Item = OsString>) -> CommandLine {
    let mut line = CommandLine::default();
    let mut args = args.into_iter();
    while let Some(arg) = args.next() {
        let Some(text) = arg.to_str() else { continue };
        let Some(option) = text.strip_prefix('-') else {
            continue;
        };
        let option = option.strip_prefix('-').unwrap_or(option);
        if option.is_empty() {
            continue;
        }
        let (name, value) = match option.split_once('=') {
            Some((name, value)) => (name, Some(value.to_owned())),
            None => (option, None),
        };
        let name = name.to_ascii_lowercase();
        match name.as_str() {
            "datadir" => {
                let value = match value {
                    Some(v) => Some(v),
                    None => args.next().and_then(|v| v.to_str().map(str::to_owned)),
                };
                if let Some(v) = value.filter(|v| !v.is_empty()) {
                    line.datadir = Some(PathBuf::from(v));
                }
            }
            "regtest" => line.regtest = value.as_deref() != Some("0"),
            name if OWNED.contains(&name) => line.refused.push(format!("-{name}")),
            // B3-Qt's own and the help options mean nothing here.
            "?" | "h" | "help" | "help-debug" | "version" | "lang" | "min" | "resetguisettings"
            | "splash" | "uiplatform" | "choosedatadir" => {}
            name => line.forwarded.push(match value {
                Some(v) => format!("-{name}={v}"),
                None => format!("-{name}"),
            }),
        }
    }
    line
}

impl CommandLine {
    /// Why the node cannot start with these options, when it cannot.
    pub fn error(&self) -> Option<String> {
        if self.refused.is_empty() {
            return None;
        }
        Some(format!(
            "B3 Hive sets {} itself. Remove {} from the command line; the data folder is -datadir, the P2P port and the rest are passed to b3coind as they are.",
            self.refused.join(", "),
            if self.refused.len() == 1 { "it" } else { "them" }
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn os(items: &[&str]) -> Vec<OsString> {
        items.iter().map(OsString::from).collect()
    }

    #[test]
    fn qt_style_options_are_forwarded_and_the_folder_taken() {
        let line = parse(os(&[
            "-datadir=D:\\b3",
            "-port=5649",
            "--listen=0",
            "-debug",
            "-Upnp=1",
            "plain",
        ]));
        assert_eq!(line.datadir, Some(PathBuf::from("D:\\b3")));
        assert_eq!(
            line.forwarded,
            ["-port=5649", "-listen=0", "-debug", "-upnp=1"]
        );
        assert!(line.refused.is_empty());
        assert!(!line.regtest);
        assert!(line.error().is_none());
        let line = parse(os(&["--datadir", "D:\\b3 data", "-regtest"]));
        assert_eq!(line.datadir, Some(PathBuf::from("D:\\b3 data")));
        assert!(line.regtest);
    }

    #[test]
    fn owned_options_are_refused_by_name() {
        let line = parse(os(&["-rpcport=1234", "-port=5649", "--server=0", "-help"]));
        assert_eq!(line.refused, ["-rpcport", "-server"]);
        assert_eq!(line.forwarded, ["-port=5649"]);
        let error = line.error().unwrap();
        assert!(error.contains("-rpcport, -server"), "{error}");
        assert!(error.contains("Remove them"), "{error}");
    }
}
