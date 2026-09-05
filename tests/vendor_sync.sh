#!/bin/sh
# vendor_sync.sh - the noise_xx implementation and Monocypher are vendored
# byte-identically into BOTH cooloo-app/server and cooloo-app/client.
# Any edit must be applied to both copies; this script enforces it.
# Run from anywhere inside the server repo; the client repo is expected
# as a sibling checkout (../client), which is how the planner repo lays
# out its submodules.
set -u

server_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
client_root=${1:-$server_root/../client}

if command -v sha256sum >/dev/null 2>&1; then
    sha() { sha256sum "$1" | cut -d' ' -f1; }
else
    sha() { shasum -a 256 "$1" | cut -d' ' -f1; }
fi

fail=0
for f in src/vendor/monocypher.c src/vendor/monocypher.h \
         src/noise_xx.c src/noise_xx.h; do
    if [ ! -f "$client_root/$f" ]; then
        echo "MISSING in client: $f"
        fail=1
        continue
    fi
    s=$(sha "$server_root/$f")
    c=$(sha "$client_root/$f")
    if [ "$s" = "$c" ]; then
        echo "OK      $f"
    else
        echo "DIFFERS $f"
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    echo "vendor copies out of sync — apply the change to both repos" >&2
    exit 1
fi
echo "vendor copies in sync"
