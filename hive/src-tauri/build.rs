use std::path::PathBuf;

fn main() {
    generate_convert_params();
    build_node_crypto();

    // tauri-build embeds the window and taskbar icon from icons/icon.ico but
    // does not watch the file, so a new icon kept the old one until a clean
    // build.
    println!("cargo:rerun-if-changed=icons/icon.ico");
    println!("cargo:rerun-if-changed=icons/icon.png");

    // Listing the app's commands makes each one a permission that a window's
    // capability must grant explicitly (capabilities/*.json).
    tauri_build::try_build(tauri_build::Attributes::new().app_manifest(
        tauri_build::AppManifest::new().commands(&[
            "node_status",
            "wallet_overview",
            "wallet_activity",
            "chain_overview",
            "network_peers",
            "dev_fund",
            "draft_send",
            "send_coins",
            "book_list",
            "book_save",
            "stake_status",
            "fm2_last",
            "fm2_pick",
            "fm2_locate",
            "fm2_connect",
            "fm2_disconnect",
            "fm2_info",
            "fm2_key_create",
            "fm2_health",
            "fm2_account",
            "fm2_watch",
            "fm2_unwatch",
            "fm2_refresh",
            "fm2_resend",
            "fm2_place_draft",
            "fm2_cancel_draft",
            "fm2_cancel_all_draft",
            "fm2_transfer_draft",
            "stake_draft",
            "release_draft",
            "wallet_restore",
            "wallet_import_legacy",
            "psbt_inspect",
            "psbt_draft",
            "psbt_save",
            "finality_draft",
            "finality_revoke_draft",
            "close_now",
            "close_cancel",
            "stake_start",
            "stake_stop",
            "open_console",
            "restart_node",
            "ui_theme",
            "set_ui_theme",
            "ui_mask",
            "set_ui_mask",
            "ui_trade_pro",
            "set_ui_trade_pro",
            "approval_show",
            "update_status",
            "update_check",
            "update_download",
            "node_info",
            "network_totals",
            "wallets",
            "wallet_manage",
            "passphrase_request",
            "wallet_lock",
            "passphrase_pending",
            "passphrase_submit",
            "passphrase_cancel",
            "receive_new",
            "receive_addresses",
            "receive_show",
            "receive_rename",
            "wallet_backup",
            "wallet_history",
            "wallet_transaction",
            "wallet_export_csv",
            "node_settings",
            "node_settings_apply",
            "datadir_choose",
            "datadir_reset",
            "setup_info",
            "setup_bootstrap",
            "setup_cancel",
            "setup_sync",
            "approval_pending",
            "approval_decide",
            "console_exec",
            "console_completions",
            "console_wallets",
        ]),
    ))
    .expect("failed to run tauri-build");
}

/// The FlowMesh V2 client signs with BIP340 and checks BLS12-381 seat
/// certificates (src/fm2/crypto.rs). It uses the node's own libraries,
/// compiled here from the repository: src/secp256k1 with the build options
/// of src/secp256k1/CMakeLists.txt, and src/blst as cmake/blst.cmake builds
/// it (upstream's assembly on x86_64 and aarch64, the pure-C path elsewhere).
fn build_node_crypto() {
    let manifest = PathBuf::from(std::env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let src = manifest.join("../../src");
    let arch = std::env::var("CARGO_CFG_TARGET_ARCH").unwrap_or_default();
    let msvc = std::env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("msvc");

    let secp = src.join("secp256k1");
    println!("cargo:rerun-if-changed={}", secp.join("src").display());
    cc::Build::new()
        .include(&secp)
        .include(secp.join("include"))
        .include(secp.join("src"))
        .define("SECP256K1_STATIC", None)
        .define("ENABLE_MODULE_EXTRAKEYS", "1")
        .define("ENABLE_MODULE_SCHNORRSIG", "1")
        .define("ECMULT_WINDOW_SIZE", "15")
        .define("COMB_BLOCKS", "43")
        .define("COMB_TEETH", "6")
        .file(secp.join("src/secp256k1.c"))
        .file(secp.join("src/precomputed_ecmult.c"))
        .file(secp.join("src/precomputed_ecmult_gen.c"))
        .warnings(false)
        .compile("b3hive_secp256k1");

    let blst = src.join("blst");
    println!("cargo:rerun-if-changed={}", blst.join("src").display());
    println!("cargo:rerun-if-changed={}", blst.join("build").display());
    let mut build = cc::Build::new();
    build.file(blst.join("src/server.c")).warnings(false);
    let with_assembly = arch == "x86_64" || (arch == "aarch64" && !msvc);
    if !with_assembly {
        build.define("__BLST_NO_ASM__", None);
    } else if msvc {
        // Upstream build.bat: every build/win64/*-x86_64.asm through ml64.
        let mut files: Vec<PathBuf> = std::fs::read_dir(blst.join("build/win64"))
            .expect("cannot list src/blst/build/win64")
            .filter_map(|entry| entry.ok().map(|e| e.path()))
            .filter(|path| {
                path.file_name()
                    .and_then(|name| name.to_str())
                    .is_some_and(|name| name.ends_with("-x86_64.asm"))
            })
            .collect();
        files.sort();
        assert!(!files.is_empty(), "no MASM sources in src/blst/build/win64");
        build.files(files).define("__BLST_PORTABLE__", None);
    } else {
        build
            .file(blst.join("build/assembly.S"))
            .define("__BLST_PORTABLE__", None)
            .flag_if_supported("-fno-builtin");
    }
    build.compile("b3hive_blst");
}

/// The console converts typed arguments exactly as b3coin-cli and the Qt
/// console do, so it reads the same table: vRPCConvertParams in
/// src/rpc/client.cpp. B3_RPC_CLIENT_CPP overrides the path. Any line the
/// parser does not understand fails the build rather than being skipped.
fn generate_convert_params() {
    let manifest = PathBuf::from(std::env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let path = std::env::var_os("B3_RPC_CLIENT_CPP")
        .map(PathBuf::from)
        .unwrap_or_else(|| manifest.join("../../src/rpc/client.cpp"));
    println!("cargo:rerun-if-env-changed=B3_RPC_CLIENT_CPP");
    println!("cargo:rerun-if-changed={}", path.display());
    let source = std::fs::read_to_string(&path)
        .unwrap_or_else(|e| panic!("cannot read {}: {e}", path.display()));

    let start = source
        .find("vRPCConvertParams[] =")
        .expect("vRPCConvertParams not found in client.cpp");
    let mut rows = Vec::new();
    for line in source[start..].lines().skip(1) {
        let line = line.trim();
        if line == "};" {
            break;
        }
        if line == "{" || line.is_empty() || line.starts_with("//") {
            continue;
        }
        rows.push(
            parse_row(line)
                .unwrap_or_else(|| panic!("unexpected line in vRPCConvertParams: {line}")),
        );
    }
    assert!(!rows.is_empty(), "vRPCConvertParams is empty");

    let mut out = String::from(
        "/// Generated by build.rs from vRPCConvertParams in src/rpc/client.cpp.\n\
         pub(crate) const CONVERT_PARAMS: &[(&str, usize, Format)] = &[\n",
    );
    for (method, index, format) in rows {
        out.push_str(&format!("    ({method:?}, {index}, Format::{format}),\n"));
    }
    out.push_str("];\n");
    let dest = PathBuf::from(std::env::var_os("OUT_DIR").unwrap()).join("convert_params.rs");
    std::fs::write(dest, out).expect("cannot write convert_params.rs");
}

/// `{ "method", 2, "name" }` or `{ "method", 2, "name", ParamFormat::STRING },`
fn parse_row(line: &str) -> Option<(String, usize, &'static str)> {
    let inner = line.strip_suffix(',').unwrap_or(line);
    let inner = inner.strip_prefix('{')?.strip_suffix('}')?;
    let fields: Vec<&str> = inner.split(',').map(str::trim).collect();
    let quoted = |s: &str| {
        s.strip_prefix('"')?
            .strip_suffix('"')
            .filter(|v| !v.is_empty() && v.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_'))
            .map(str::to_owned)
    };
    let method = quoted(fields.first()?)?;
    let index = fields.get(1)?.parse().ok()?;
    quoted(fields.get(2)?)?;
    let format = match fields.get(3).copied() {
        None | Some("ParamFormat::JSON") => "Json",
        Some("ParamFormat::STRING") => "String",
        Some("ParamFormat::JSON_OR_STRING") => "JsonOrString",
        Some(_) => return None,
    };
    if fields.len() > 4 {
        return None;
    }
    Some((method, index, format))
}
