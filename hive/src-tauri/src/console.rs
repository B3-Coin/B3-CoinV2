//! The console's command line: a port of RPCConsole::RPCParseCommandLine
//! (src/qt/rpcconsole.cpp), so commands typed here behave as in the Qt
//! console. That covers nesting with parentheses, quoting and escapes,
//! result queries such as `[tx][0]`, and the history filter that hides the
//! arguments of commands handling secrets. Arguments are converted with the
//! same table as b3coin-cli (vRPCConvertParams, read by build.rs).

use std::fmt;

use serde_json::Value;

#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) enum Format {
    Json,
    String,
    JsonOrString,
}

include!(concat!(env!("OUT_DIR"), "/convert_params.rs"));

/// Commands whose arguments never reach the history (Qt's historyFilter).
const HISTORY_FILTER: &[&str] = &[
    "createwallet",
    "createwalletdescriptor",
    "getdescriptorinfo",
    "importdescriptors",
    "importfinalitykey",
    "importflowmeshkey",
    "exportflowmeshkey",
    "importlegacywalletdump",
    "migratewallet",
    "signmessagewithprivkey",
    "signrawtransactionwithkey",
    "walletpassphrase",
    "walletpassphrasechange",
    "encryptwallet",
];

/// The Qt console's `help-console` text.
pub const HELP_CONSOLE: &str = "\n\
This console accepts RPC commands using the standard syntax.\n   \
example:    getblockhash 0\n\n\
This console can also accept RPC commands using the parenthesized syntax.\n   \
example:    getblockhash(0)\n\n\
Commands may be nested when specified with the parenthesized syntax.\n   \
example:    getblock(getblockhash(0) 1)\n\n\
A space or a comma can be used to delimit arguments for either syntax.\n   \
example:    getblockhash 0\n               \
getblockhash,0\n\n\
Named results can be queried with a non-quoted key string in brackets using the parenthesized syntax.\n   \
example:    getblock(getblockhash(0) 1)[tx]\n\n\
Results without keys can be queried with an integer in brackets using the parenthesized syntax.\n   \
example:    getblock(getblockhash(0),1)[tx][0]\n\n";

#[derive(Debug, Clone, PartialEq)]
pub enum ConsoleError {
    /// A malformed line or failed conversion (Qt: std::runtime_error).
    Error(String),
    /// The line ends inside quotes or after an escape.
    Unbalanced,
    /// The node answered with a JSON-RPC error.
    Rpc { code: i64, message: String },
}

/// The messages match the Qt console's replies.
impl fmt::Display for ConsoleError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ConsoleError::Error(e) => write!(f, "Error: {e}"),
            ConsoleError::Unbalanced => write!(f, "Parse error: unbalanced ' or \""),
            ConsoleError::Rpc { code, message } => write!(f, "{message} (code {code})"),
        }
    }
}

fn syntax_error() -> ConsoleError {
    ConsoleError::Error("Invalid Syntax".into())
}

/// Runs one RPC: method name and converted params in, result out.
pub type Executor<'a> = dyn FnMut(&str, Value) -> Result<Value, ConsoleError> + 'a;

/// What the console shows for a submitted line.
#[derive(Debug, Clone, PartialEq, serde::Serialize)]
pub struct Reply {
    /// The line as it may be echoed and kept in history (secrets replaced).
    pub command: String,
    pub ok: bool,
    pub output: String,
}

/// Handles a submitted line as the Qt console does: refuse it if it cannot
/// even be filtered for the history, answer `help-console` locally, and
/// otherwise execute it. `Err` means nothing ran and the line should stay
/// in the input box.
pub fn submit(line: &str, exec: &mut Executor<'_>) -> Result<Reply, String> {
    let line = line.trim();
    let command = match parse(line, None) {
        Ok(parsed) => parsed.filtered,
        Err(ConsoleError::Unbalanced) => return Err("Error: Invalid command line".into()),
        Err(e) => return Err(e.to_string()),
    };
    if line == "help-console" {
        return Ok(Reply {
            command,
            ok: true,
            output: HELP_CONSOLE.into(),
        });
    }
    Ok(match parse(&format!("{line}\n"), Some(exec)) {
        Ok(parsed) => Reply {
            command,
            ok: true,
            output: parsed.result,
        },
        Err(e) => Reply {
            command,
            ok: false,
            output: e.to_string(),
        },
    })
}

/// True if `line` is exactly the stop command, which the console lets
/// through even while another command runs.
pub fn is_stop(line: &str) -> bool {
    line.trim() == "stop"
}

#[derive(Debug, PartialEq)]
pub struct Parsed {
    pub result: String,
    pub filtered: String,
}

#[derive(Clone, Copy, PartialEq)]
enum State {
    EatingSpaces,
    EatingSpacesInArg,
    EatingSpacesInBrackets,
    Argument,
    SingleQuoted,
    DoubleQuoted,
    EscapeOuter,
    EscapeDoubleQuoted,
    CommandExecuted,
    CommandExecutedInner,
}

/// The command stack and history-filter bookkeeping (Qt's lambdas).
struct Stack {
    frames: Vec<Vec<String>>,
    depth_inside_sensitive: u32,
    filter_begin: usize,
    filter_ranges: Vec<(usize, usize)>,
}

impl Stack {
    fn add(&mut self, arg: String, chpos: usize) {
        let starts_command = self.frames.last().map_or(true, Vec::is_empty);
        if starts_command
            && self.depth_inside_sensitive == 0
            && HISTORY_FILTER.iter().any(|c| c.eq_ignore_ascii_case(&arg))
        {
            self.depth_inside_sensitive = 1;
            self.filter_begin = chpos;
        }
        if self.frames.is_empty() {
            self.frames.push(Vec::new());
        }
        self.frames.last_mut().unwrap().push(arg);
    }

    fn close(&mut self, chpos: usize) {
        if self.depth_inside_sensitive > 0 {
            self.depth_inside_sensitive -= 1;
            if self.depth_inside_sensitive == 0 {
                self.filter_ranges.push((self.filter_begin, chpos));
                self.filter_begin = 0;
            }
        }
        self.frames.pop();
    }
}

/// Splits and optionally executes a command line, state for state as
/// RPCParseCommandLine does. Without an executor it only parses, which is
/// how the history filter is computed.
pub fn parse(command: &str, mut exec: Option<&mut Executor<'_>>) -> Result<Parsed, ConsoleError> {
    let mut stack = Stack {
        frames: vec![Vec::new()],
        depth_inside_sensitive: 0,
        filter_begin: 0,
        filter_ranges: Vec::new(),
    };
    let mut state = State::EatingSpaces;
    let mut curarg: Vec<u8> = Vec::new();
    let mut last_result = Value::Null;
    let mut result = String::new();

    let mut terminated = command.to_owned();
    if !terminated.ends_with('\n') {
        terminated.push('\n');
    }
    // Splits happen only at ASCII bytes, so every argument stays valid UTF-8.
    let take = |arg: &mut Vec<u8>| String::from_utf8(std::mem::take(arg)).expect("split at ASCII");
    let bytes = terminated.as_bytes();

    for (chpos, &ch) in bytes.iter().enumerate() {
        if matches!(state, State::CommandExecuted | State::CommandExecutedInner) {
            if ch == b'[' {
                curarg.clear();
                state = State::CommandExecutedInner;
                continue;
            }
            if state == State::CommandExecutedInner {
                if ch != b']' {
                    curarg.push(ch);
                    continue;
                }
                if !curarg.is_empty() && exec.is_some() {
                    let key = String::from_utf8_lossy(&curarg).into_owned();
                    last_result = match &last_result {
                        Value::Array(items) => {
                            let index = parse_index(&key).ok_or_else(|| {
                                ConsoleError::Error("Invalid result query".into())
                            })?;
                            items.get(index).cloned().unwrap_or(Value::Null)
                        }
                        Value::Object(map) => map.get(&key).cloned().unwrap_or(Value::Null),
                        _ => return Err(ConsoleError::Error("Invalid result query".into())),
                    };
                }
                state = State::CommandExecuted;
                continue;
            }
            // Any other character: hand the result to the enclosing command
            // (or keep it as the line's result), then parse ch normally.
            stack.close(chpos);
            let text = render(&last_result);
            if !text.is_empty() {
                if stack.frames.is_empty() {
                    result = text;
                } else {
                    stack.add(text, chpos);
                }
            }
            curarg.clear();
            state = State::EatingSpaces;
        }

        match state {
            State::Argument
            | State::EatingSpacesInArg
            | State::EatingSpacesInBrackets
            | State::EatingSpaces => match ch {
                b'"' => state = State::DoubleQuoted,
                b'\'' => state = State::SingleQuoted,
                b'\\' => state = State::EscapeOuter,
                b'(' | b')' | b'\n' => {
                    if state == State::EatingSpacesInArg {
                        return Err(syntax_error());
                    }
                    if state == State::Argument {
                        if ch == b'(' && stack.frames.last().is_some_and(|f| !f.is_empty()) {
                            if stack.depth_inside_sensitive > 0 {
                                stack.depth_inside_sensitive += 1;
                            }
                            stack.frames.push(Vec::new());
                        }
                        // No commands after an executed command at the base level.
                        if stack.frames.is_empty() {
                            return Err(syntax_error());
                        }
                        stack.add(take(&mut curarg), chpos);
                        state = State::EatingSpacesInBrackets;
                    }
                    if (ch == b')' || ch == b'\n') && !stack.frames.is_empty() {
                        if let Some(exec) = exec.as_mut() {
                            let frame = stack.frames.last().unwrap();
                            // Qt reads frame[0] unchecked; an empty call is a syntax error here.
                            let (method, args) = frame.split_first().ok_or_else(syntax_error)?;
                            let params = convert_values(method, args)?;
                            last_result = exec(method, params)?;
                        }
                        state = State::CommandExecuted;
                        curarg.clear();
                    }
                }
                b' ' | b',' | b'\t' => {
                    if state == State::EatingSpacesInArg && curarg.is_empty() && ch == b',' {
                        return Err(syntax_error());
                    } else if state == State::Argument {
                        stack.add(take(&mut curarg), chpos);
                    }
                    if (state == State::EatingSpacesInBrackets || state == State::Argument)
                        && ch == b','
                    {
                        state = State::EatingSpacesInArg;
                    } else {
                        state = State::EatingSpaces;
                    }
                }
                _ => {
                    curarg.push(ch);
                    state = State::Argument;
                }
            },
            State::SingleQuoted => match ch {
                b'\'' => state = State::Argument,
                _ => curarg.push(ch),
            },
            State::DoubleQuoted => match ch {
                b'"' => state = State::Argument,
                b'\\' => state = State::EscapeDoubleQuoted,
                _ => curarg.push(ch),
            },
            State::EscapeOuter => {
                curarg.push(ch);
                state = State::Argument;
            }
            State::EscapeDoubleQuoted => {
                // Keep the backslash for everything but a quote and a backslash.
                if ch != b'"' && ch != b'\\' {
                    curarg.push(b'\\');
                }
                curarg.push(ch);
                state = State::DoubleQuoted;
            }
            State::CommandExecuted | State::CommandExecutedInner => unreachable!(),
        }
    }

    if state == State::CommandExecuted && !stack.frames.is_empty() {
        stack.close(bytes.len());
    }
    let mut filtered = command.to_owned();
    for &(begin, end) in stack.filter_ranges.iter().rev() {
        let end = end.min(filtered.len());
        filtered.replace_range(begin..end, "(…)");
    }

    match state {
        State::CommandExecuted => Ok(Parsed {
            result: render(&last_result),
            filtered,
        }),
        State::Argument | State::EatingSpaces => Ok(Parsed { result, filtered }),
        _ => Err(ConsoleError::Unbalanced),
    }
}

/// Strings print bare, everything else as JSON indented by 2 (UniValue::write(2)).
fn render(value: &Value) -> String {
    match value {
        Value::String(s) => s.clone(),
        other => serde_json::to_string_pretty(other).expect("JSON values serialize"),
    }
}

/// Like ToIntegral<size_t>: decimal digits only, no sign.
fn parse_index(s: &str) -> Option<usize> {
    if s.is_empty() || !s.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    s.parse().ok()
}

/// RPCConvertValues: listed parameters are parsed as JSON, the rest stay strings.
pub fn convert_values(method: &str, args: &[String]) -> Result<Value, ConsoleError> {
    let mut params = Vec::with_capacity(args.len());
    for (index, raw) in args.iter().enumerate() {
        let format = CONVERT_PARAMS
            .iter()
            .find(|(m, i, _)| *m == method && *i == index)
            .map(|(_, _, f)| *f);
        params.push(match format {
            Some(Format::Json) => serde_json::from_str(raw)
                .map_err(|_| ConsoleError::Error(format!("Error parsing JSON: {raw}")))?,
            Some(Format::JsonOrString) => {
                serde_json::from_str(raw).unwrap_or_else(|_| Value::String(raw.clone()))
            }
            Some(Format::String) | None => Value::String(raw.clone()),
        });
    }
    Ok(Value::Array(params))
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    const BEST: &str = "0f9188f13cb7b2c71f2a335e3a4fc328bf5beb436012afca590b1a11466e2206";
    const COINBASE: &str = "4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b";

    /// A stand-in node with the calls Qt's rpcnestedtests use.
    fn node(method: &str, params: Value) -> Result<Value, ConsoleError> {
        let args = params.as_array().unwrap();
        match method {
            "rpcNestedTest" => Ok(Value::String(serde_json::to_string(&params).unwrap())),
            "getblockchaininfo" if args.is_empty() => Ok(json!({"chain": "main", "blocks": 0})),
            "getbestblockhash" => Ok(json!(BEST)),
            "getblock" if args.first() == Some(&json!(BEST)) => {
                Ok(json!({"hash": BEST, "tx": [COINBASE]}))
            }
            "createrawtransaction" => Ok(Value::String(serde_json::to_string(&params).unwrap())),
            "getblockchaininfo" | "getblock" => Err(ConsoleError::Rpc {
                code: -1,
                message: format!("bad params for {method}"),
            }),
            _ => Err(ConsoleError::Rpc {
                code: -32601,
                message: "Method not found".into(),
            }),
        }
    }

    fn run(line: &str) -> Result<String, ConsoleError> {
        parse(line, Some(&mut node)).map(|p| p.result)
    }

    fn filtered(line: &str) -> String {
        parse(line, None).unwrap().filtered
    }

    #[test]
    fn nesting_and_result_queries_match_qt() {
        assert_eq!(run("getblockchaininfo()[chain]").unwrap(), "main");
        assert_eq!(
            filtered("getblockchaininfo()[chain]"),
            "getblockchaininfo()[chain]"
        );
        assert!(run("getblock(getbestblockhash())")
            .unwrap()
            .starts_with('{'));
        assert!(run("getblock(getblock(getbestblockhash())[hash], true)").is_ok());
        assert!(
            run("getblock( getblock( getblock(getbestblockhash())[hash] )[hash], true)").is_ok()
        );
        for line in [
            "getblockchaininfo",
            "getblockchaininfo()",
            "getblockchaininfo ",
        ] {
            assert!(run(line).unwrap().starts_with('{'), "{line}");
        }
        assert_eq!(run("getblockchaininfo()[\"chain\"]").unwrap(), "null");
        assert_eq!(
            run("getblock(getbestblockhash())[tx][0]").unwrap(),
            COINBASE
        );
    }

    #[test]
    fn argument_splitting_matches_qt() {
        let a = run("createrawtransaction [] {} 0").unwrap();
        assert_eq!(a, "[[],{},0]");
        assert_eq!(run("createrawtransaction([],{},0)").unwrap(), a);
        assert_eq!(run("createrawtransaction( [],  {} , 0   )").unwrap(), a);
        for (line, want) in [
            ("rpcNestedTest", "[]"),
            ("rpcNestedTest ''", "[\"\"]"),
            ("rpcNestedTest \"\"", "[\"\"]"),
            ("rpcNestedTest '' abc", "[\"\",\"abc\"]"),
            ("rpcNestedTest abc '' abc", "[\"abc\",\"\",\"abc\"]"),
            ("rpcNestedTest abc  abc", "[\"abc\",\"abc\"]"),
            ("rpcNestedTest abc\t\tabc", "[\"abc\",\"abc\"]"),
            ("rpcNestedTest(abc )", "[\"abc\"]"),
            ("rpcNestedTest( abc )", "[\"abc\"]"),
            ("rpcNestedTest(   abc   ,   cba )", "[\"abc\",\"cba\"]"),
            (
                "rpcNestedTest \"a\\\"b\" 'c\\d' e\\ f",
                "[\"a\\\"b\",\"c\\\\d\",\"e f\"]",
            ),
        ] {
            assert_eq!(run(line).unwrap(), want, "{line}");
        }
    }

    #[test]
    fn errors_match_qt() {
        for line in [
            "getblockchaininfo() .\n",
            "getblockchaininfo() getblockchaininfo()",
            "rpcNestedTest abc,,abc",
            "rpcNestedTest(abc,,abc)",
            "rpcNestedTest(abc,,)",
        ] {
            assert_eq!(run(line), Err(syntax_error()), "{line}");
        }
        assert!(run("getblockchaininfo(").is_ok());
        assert!(run("getblockchaininfo()()()").is_ok());
        assert!(matches!(
            run("getblockchaininfo(True)"),
            Err(ConsoleError::Rpc { .. })
        ));
        assert!(matches!(
            run("a(getblockchaininfo(True))"),
            Err(ConsoleError::Rpc { .. })
        ));
        assert_eq!(run("rpcNestedTest 'abc"), Err(ConsoleError::Unbalanced));
        assert_eq!(
            run("getblockchaininfo()[x]").unwrap(),
            "null",
            "missing keys give null"
        );
        assert_eq!(
            run("getblock(getbestblockhash())[tx][+0]"),
            Err(ConsoleError::Error("Invalid result query".into()))
        );
    }

    #[test]
    fn secrets_never_reach_the_history() {
        for (line, want) in [
            ("createwallet test true", "createwallet(…)"),
            ("createwalletdescriptor abc", "createwalletdescriptor(…)"),
            ("migratewallet abc abc", "migratewallet(…)"),
            ("getdescriptorinfo combo(secret)", "getdescriptorinfo(…)"),
            ("importdescriptors private", "importdescriptors(…)"),
            ("importfinalitykey secret", "importfinalitykey(…)"),
            ("importflowmeshkey secret", "importflowmeshkey(…)"),
            ("exportflowmeshkey public true", "exportflowmeshkey(…)"),
            (
                "importlegacywalletdump /secret/path",
                "importlegacywalletdump(…)",
            ),
            ("signmessagewithprivkey abc", "signmessagewithprivkey(…)"),
            (
                "signmessagewithprivkey abc,def",
                "signmessagewithprivkey(…)",
            ),
            (
                "signrawtransactionwithkey(abc)",
                "signrawtransactionwithkey(…)",
            ),
            ("walletpassphrase(help())", "walletpassphrase(…)"),
            (
                "walletpassphrasechange(help(walletpassphrasechange(abc)))",
                "walletpassphrasechange(…)",
            ),
            ("help(encryptwallet(abc, def))", "help(encryptwallet(…))"),
            ("WalletPassphrase secret 60", "WalletPassphrase(…)"),
        ] {
            assert_eq!(filtered(line), want, "{line}");
        }
    }

    #[test]
    fn arguments_convert_with_the_cli_table() {
        assert!(!CONVERT_PARAMS.is_empty());
        let p = convert_values("getblock", &["abc".into(), "2".into()]).unwrap();
        assert_eq!(p, json!(["abc", 2]));
        // Listed as STRING: stays a string even though it parses as JSON.
        let p = convert_values("setlabel", &["addr".into(), "123".into()]).unwrap();
        assert_eq!(p, json!(["addr", "123"]));
        // Unlisted parameters are never parsed.
        let p = convert_values("getnewaddress", &["true".into()]).unwrap();
        assert_eq!(p, json!(["true"]));
        assert_eq!(
            convert_values("getblock", &["abc".into(), "two".into()]),
            Err(ConsoleError::Error("Error parsing JSON: two".into()))
        );
        // Amounts keep their exact digits.
        let p = convert_values("sendtoaddress", &["a".into(), "0.123456789".into()]).unwrap();
        assert_eq!(p[1].to_string(), "0.123456789");
    }

    #[test]
    fn submit_filters_before_running_and_answers_help_locally() {
        let mut ran = Vec::new();
        let mut exec = |method: &str, _params: Value| {
            ran.push(method.to_owned());
            Ok(Value::Null)
        };
        let reply = submit("  walletpassphrase hunter2 60 ", &mut exec).unwrap();
        assert_eq!(reply.command, "walletpassphrase(…)");
        assert_eq!((reply.ok, reply.output.as_str()), (true, "null"));
        assert_eq!(
            submit("help-console", &mut exec).unwrap().output,
            HELP_CONSOLE
        );
        assert_eq!(
            submit("getinfo 'abc", &mut exec),
            Err("Error: Invalid command line".into())
        );
        assert_eq!(
            submit("a,,b", &mut exec),
            Err("Error: Invalid Syntax".into())
        );
        assert_eq!(ran, ["walletpassphrase"]);
        assert!(is_stop(" stop ") && !is_stop("stop now"));
    }
}
