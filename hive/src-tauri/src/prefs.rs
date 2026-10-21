//! The main window's display choices: the theme and whether amounts are
//! hidden. The windows are incognito, so their own storage is gone when B3
//! Hive closes; these are kept in B3 Hive's config folder instead. Nothing
//! else is stored here.

use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::Theme;

#[derive(Clone, Copy, Default, Serialize, Deserialize)]
pub struct Prefs {
    /// None follows the system's light or dark setting.
    #[serde(default)]
    pub theme: Option<Theme>,
    /// Qt's "Mask values".
    #[serde(default)]
    pub mask: bool,
}

pub fn path(config_dir: &Path) -> PathBuf {
    config_dir.join("display.json")
}

/// The saved choices; the defaults when there are none or they cannot be
/// read, since nothing depends on them but the look.
pub fn read(path: &Path) -> Prefs {
    std::fs::read(path)
        .ok()
        .and_then(|bytes| serde_json::from_slice(&bytes).ok())
        .unwrap_or_default()
}

/// Saves through a temporary file, so a crash leaves the old or the new
/// choices, never half of them.
pub fn write(path: &Path, prefs: &Prefs) -> Result<(), String> {
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)
            .map_err(|e| format!("Cannot create {}: {e}", dir.display()))?;
    }
    let text = serde_json::to_vec_pretty(prefs).map_err(|e| e.to_string())?;
    let temp = path.with_extension("json.tmp");
    std::fs::write(&temp, text).map_err(|e| format!("Cannot write {}: {e}", temp.display()))?;
    std::fs::rename(&temp, path).map_err(|e| format!("Cannot replace {}: {e}", path.display()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn choices_survive_a_write_and_bad_files_give_defaults() {
        let dir = std::env::temp_dir().join(format!("hive-prefs-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let file = path(&dir);
        assert!(read(&file).theme.is_none());
        write(
            &file,
            &Prefs {
                theme: Some(Theme::Light),
                mask: true,
            },
        )
        .unwrap();
        let back = read(&file);
        assert!(matches!(back.theme, Some(Theme::Light)));
        assert!(back.mask);
        std::fs::write(&file, "not json").unwrap();
        assert!(!read(&file).mask);
        let _ = std::fs::remove_dir_all(&dir);
    }
}
