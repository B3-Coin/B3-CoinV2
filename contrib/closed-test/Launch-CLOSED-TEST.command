#!/bin/sh
# Sibling .app only; no options, alternate executable or wallet/datadir override.
# Keep this script beside B3 FlowMesh CLOSED TEST.app when packaging.
umask 077
if [ "$#" -ne 0 ]; then
    printf '%s\n' 'CLOSED TEST launcher accepts no arguments.' >&2
    exit 64
fi
case "$0" in
    /*) launcher_path=$0 ;;
    *) launcher_path=$PWD/$0 ;;
esac
launch_dir=$(CDPATH= cd -- "$(dirname -- "$launcher_path")" && pwd -P) || exit 70
app_path="$launch_dir/B3 FlowMesh CLOSED TEST.app/Contents/MacOS/test_b3_flowmeshclosed-gui"
if [ ! -f "$app_path" ] || [ ! -x "$app_path" ] || [ -L "$app_path" ]; then
    printf '%s\n' 'The matching CLOSED TEST app is missing. Do not substitute a normal wallet app.' >&2
    exit 66
fi
attempt_dir=$(mktemp -d "$launch_dir/closed-test-attempt.XXXXXXXX") || {
    printf '%s\n' 'Cannot create a private capture folder. Copy the test package to a writable local folder first.' >&2
    exit 73
}
/usr/bin/shasum -a 256 "$app_path" > "$attempt_dir/executable-sha256.txt" || exit 74
/bin/date -u '+%Y-%m-%dT%H:%M:%SZ' > "$attempt_dir/started-utc.txt"
printf 'CLOSED TEST attempt record: %s\n' "$attempt_dir"
"$app_path" > "$attempt_dir/stdout.log" 2> "$attempt_dir/stderr.log"
child_status=$?
printf '%s\n' "$child_status" > "$attempt_dir/exit-status.txt"
/bin/date -u '+%Y-%m-%dT%H:%M:%SZ' > "$attempt_dir/ended-utc.txt"
printf 'CLOSED TEST child exit status: %s\nPrivate local logs: %s\n' "$child_status" "$attempt_dir"
printf '%s\n' 'No logs were uploaded. Review identifying details before privately sharing a bounded excerpt.'
exit "$child_status"
