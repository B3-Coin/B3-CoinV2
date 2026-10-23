//! The data folder picked on the Settings page, kept as folder.json in B3
//! Hive's config folder. It is read once, when B3 Hive starts, and takes
//! effect then: a running node cannot move. A `--datadir` argument or
//! B3_HIVE_DATADIR in the environment outranks it for that start.

use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize)]
struct File {
    datadir: PathBuf,
}

pub fn path(config_dir: &Path) -> PathBuf {
    config_dir.join("folder.json")
}

/// The saved folder, or None when there is no choice or it cannot be read:
/// the default folder is then used, and the Settings page shows that.
pub fn read(path: &Path) -> Option<PathBuf> {
    let bytes = std::fs::read(path).ok()?;
    let file: File = serde_json::from_slice(&bytes).ok()?;
    Some(file.datadir)
}

/// Saves the choice through a temporary file, or removes it for None.
pub fn write(path: &Path, datadir: Option<&Path>) -> Result<(), String> {
    let Some(datadir) = datadir else {
        return match std::fs::remove_file(path) {
            Ok(()) => Ok(()),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
            Err(e) => Err(format!("Cannot remove {}: {e}", path.display())),
        };
    };
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)
            .map_err(|e| format!("Cannot create {}: {e}", dir.display()))?;
    }
    let text = serde_json::to_vec_pretty(&File {
        datadir: datadir.to_path_buf(),
    })
    .map_err(|e| e.to_string())?;
    let temp = path.with_extension("json.tmp");
    std::fs::write(&temp, text).map_err(|e| format!("Cannot write {}: {e}", temp.display()))?;
    std::fs::rename(&temp, path).map_err(|e| format!("Cannot replace {}: {e}", path.display()))
}

/// The data folder for this start and where the choice came from, in order
/// of precedence: a `-datadir` argument (cli.rs), B3_HIVE_DATADIR, the saved
/// choice, then `default`.
pub fn resolve(
    argument: Option<PathBuf>,
    environment: Option<std::ffi::OsString>,
    saved: Option<PathBuf>,
    default: PathBuf,
) -> (PathBuf, Source) {
    if let Some(dir) = argument {
        return (dir, Source::Argument);
    }
    if let Some(dir) = environment {
        return (PathBuf::from(dir), Source::Environment);
    }
    if let Some(dir) = saved {
        return (dir, Source::Preference);
    }
    (default, Source::Default)
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Source {
    Argument,
    Environment,
    Preference,
    Default,
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn argument_outranks_environment_outranks_choice_outranks_default() {
        let default = PathBuf::from("default");
        let saved = Some(PathBuf::from("saved"));
        assert_eq!(
            resolve(Some("arg".into()), Some("env".into()), saved.clone(), default.clone()),
            (PathBuf::from("arg"), Source::Argument)
        );
        assert_eq!(
            resolve(None, Some("env".into()), saved.clone(), default.clone()),
            (PathBuf::from("env"), Source::Environment)
        );
        assert_eq!(
            resolve(None, None, saved, default.clone()),
            (PathBuf::from("saved"), Source::Preference)
        );
        assert_eq!(
            resolve(None, None, None, default.clone()),
            (default, Source::Default)
        );
    }

    #[test]
    fn choice_round_trips_and_none_removes_it() {
        let dir = std::env::temp_dir().join(format!("hive-folder-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let file = path(&dir);
        assert!(read(&file).is_none());
        write(&file, Some(Path::new("C:\\b3\\data"))).unwrap();
        assert_eq!(read(&file), Some(PathBuf::from("C:\\b3\\data")));
        write(&file, None).unwrap();
        assert!(!file.exists());
        write(&file, None).unwrap();
        std::fs::write(&file, "not json").unwrap();
        assert!(read(&file).is_none());
        let _ = std::fs::remove_dir_all(&dir);
    }
}
