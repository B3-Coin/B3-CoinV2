#!/usr/bin/env bash
# Copies a b3coind build into src-tauri/binaries/ under the name Tauri expects
# for a sidecar (b3coind-<target triple>), stripped of debug symbols. Run it
# before `npm run package`; the bundle installs it next to the app.
#
#   scripts/stage-b3coind.sh /path/to/b3coind
set -euo pipefail
src=${1:?usage: $0 /path/to/b3coind}
[ -x "$src" ] || { echo "not an executable: $src" >&2; exit 1; }
triple=$(rustc -vV | sed -n 's/^host: //p')
dest="$(dirname "$0")/../src-tauri/binaries/b3coind-$triple"
mkdir -p "$(dirname "$dest")"
strip --strip-debug -o "$dest" "$src"
ls -l "$dest"
