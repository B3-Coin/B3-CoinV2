fn main() {
    // Listing the app's commands makes each one a permission that a window's
    // capability must grant explicitly (capabilities/*.json).
    tauri_build::try_build(
        tauri_build::Attributes::new()
            .app_manifest(tauri_build::AppManifest::new().commands(&["node_status"])),
    )
    .expect("failed to run tauri-build");
}
