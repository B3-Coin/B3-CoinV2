#!/usr/bin/env bash
# Copies a program into src-tauri/binaries/ under the name Tauri expects for
# a sidecar (<name>-<target triple>[.exe]), stripped of debug symbols. Run it
# for both sidecars before `npm run package`; the bundle installs each next
# to the app under its plain name:
#   b3-hive-node     the b3coind build (so it never clashes with an
#                    installed b3coind)
#   b3-hive-update   the update helper, built with -DB3_HIVE_UPDATER=ON and
#                    the pinned channel (contrib/b3hive-release/)
#
#   scripts/stage-sidecar.sh <name> /path/to/program [target-triple]
#
# The triple defaults to rustc's host. For a Windows app, stage the MinGW
# builds with the app's triple and the MinGW strip, for example from WSL:
#   STRIP=x86_64-w64-mingw32-strip scripts/stage-sidecar.sh b3-hive-node b3coind.exe x86_64-pc-windows-msvc
# STRIP=none copies the file as it is, for a binary stripped where it was built.
set -euo pipefail
name=${1:?usage: $0 <name> /path/to/program [target-triple]}
src=${2:?usage: $0 <name> /path/to/program [target-triple]}
case "$name" in
  b3-hive-node | b3-hive-update) ;;
  *) echo "unknown sidecar: $name" >&2; exit 1 ;;
esac
[ -f "$src" ] || { echo "no such file: $src" >&2; exit 1; }
triple=${3:-$(rustc -vV | sed -n 's/^host: //p')}
ext=""
case "$src" in *.exe) ext=".exe" ;; esac
dest="$(dirname "$0")/../src-tauri/binaries/$name-$triple$ext"
mkdir -p "$(dirname "$dest")"
if [ "${STRIP:-strip}" = none ]; then
  cp "$src" "$dest"
else
  "${STRIP:-strip}" --strip-debug -o "$dest" "$src"
fi
chmod +x "$dest"
ls -l "$dest"
