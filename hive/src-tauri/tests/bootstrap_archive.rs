//! Installs a real explorer bootstrap from a local file, as the setup wizard
//! would after downloading it. Runs only when given one:
//!   B3_HIVE_BOOTSTRAP_ARCHIVE=/path/bootstrap-860000.tar.zst \
//!   B3_HIVE_BOOTSTRAP_SHA256=<manifest sha256> \
//!   B3_HIVE_BOOTSTRAP_TARGET=/empty/folder cargo test --release --test bootstrap_archive -- --ignored
//! The target keeps the installed chain, so b3coind can be pointed at it
//! (offline: -connect=0 -listen=0 -dnsseed=0).

use std::fs::File;
use std::path::PathBuf;
use std::sync::atomic::AtomicBool;

use b3_hive::bootstrap;

#[test]
#[ignore = "needs a bootstrap archive: set B3_HIVE_BOOTSTRAP_* and pass --ignored"]
fn installs_a_real_bootstrap() {
    let (Some(archive), Ok(sha256), Some(target)) = (
        std::env::var_os("B3_HIVE_BOOTSTRAP_ARCHIVE").map(PathBuf::from),
        std::env::var("B3_HIVE_BOOTSTRAP_SHA256"),
        std::env::var_os("B3_HIVE_BOOTSTRAP_TARGET").map(PathBuf::from),
    ) else {
        panic!("B3_HIVE_BOOTSTRAP_ARCHIVE, _SHA256 and _TARGET must all be set");
    };
    let size = std::fs::metadata(&archive).unwrap().len();
    let name = archive.file_name().unwrap().to_str().unwrap().to_owned();
    let height: u64 = name
        .strip_prefix("bootstrap-")
        .and_then(|n| n.strip_suffix(".tar.zst"))
        .and_then(|n| n.parse().ok())
        .expect("named bootstrap-<height>.tar.zst");
    let offer = bootstrap::parse_manifest(
        format!(
            r#"{{"bootstraps": [{{"height": {height}, "size": {size}, "sha256": "{sha256}",
                "url": "/bootstraps/{name}"}}]}}"#
        )
        .as_bytes(),
    )
    .unwrap()
    .pop()
    .expect("the archive's details are well formed");

    let mut last = None;
    bootstrap::install(
        &mut File::open(&archive).unwrap(),
        &offer,
        &target,
        &mut |p| last = Some(p),
        &AtomicBool::new(false),
    )
    .unwrap();
    assert_eq!(last, Some(bootstrap::Progress::Done { height }));
    for folder in ["blocks", "chainstate"] {
        assert!(target.join(folder).is_dir(), "{folder} missing");
    }
    assert!(!bootstrap::is_fresh(&target));
}
