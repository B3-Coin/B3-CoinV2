#!/bin/sh
# Regenerate the shared bridge V2 vectors (contracts spec §2.5), or check them.
#
#   gen_v2_vectors.sh            rewrite bridge_v2_vectors.json beside this script
#   gen_v2_vectors.sh --check    exit 1 unless the committed file is up to date
#
# Any other arguments go to gen_v2_vectors.py unchanged.
set -eu
cd "$(dirname "$0")"
if [ $# -eq 0 ]; then
    set -- --out bridge_v2_vectors.json
fi
exec python3 -B gen_v2_vectors.py "$@"
