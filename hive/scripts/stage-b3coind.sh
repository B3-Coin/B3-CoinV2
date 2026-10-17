#!/usr/bin/env bash
# Copies a b3coind build into src-tauri/binaries/ under the name Tauri expects
# for a sidecar (b3-hive-node-<target triple>[.exe]), stripped of debug
# symbols. Run it before `npm run package`. The bundle installs it next to the
# app as b3-hive-node, so it never clashes with an installed b3coind.
#
#   scripts/stage-b3coind.sh /path/to/b3coind [target-triple]
#
# The triple defaults to rustc's host. For a Windows app, stage the MinGW
# b3coind.exe with the app's triple and the MinGW strip, for example from WSL:
#   STRIP=x86_64-w64-mingw32-strip scripts/stage-b3coind.sh b3coind.exe x86_64-pc-windows-msvc
set -euo pipefail
src=${1:?usage: $0 /path/to/b3coind [target-triple]}
[ -f "$src" ] || { echo "no such file: $src" >&2; exit 1; }
triple=${2:-$(rustc -vV | sed -n 's/^host: //p')}
ext=""
case "$src" in *.exe) ext=".exe" ;; esac
dest="$(dirname "$0")/../src-tauri/binaries/b3-hive-node-$triple$ext"
mkdir -p "$(dirname "$dest")"
"${STRIP:-strip}" --strip-debug -o "$dest" "$src"
chmod +x "$dest"
ls -l "$dest"
