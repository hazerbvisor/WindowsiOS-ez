#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
UDID="${1:?Usage: launch_on_duo.sh DEVICE_UDID [SIGNED_BUNDLE_ID]}"
BUNDLE_ID="${2:-io.github.hazerbvisor.windowsios-ez}"
LAUNCH_JSON="$ROOT/work/duo_launch.json"

mkdir -p "$ROOT/work"
rm -f "$LAUNCH_JSON"

echo "[duo] Launching $BUNDLE_ID (--start-stopped) on $UDID..."
for attempt in 1 2 3 4 5; do
    if xcrun devicectl device process launch \
        --start-stopped \
        --terminate-existing \
        --device "$UDID" \
        --json-output "$LAUNCH_JSON" \
        "$BUNDLE_ID" || \
       xcrun devicectl device process launch \
        --no-activate \
        --start-stopped \
        --terminate-existing \
        --device "$UDID" \
        --json-output "$LAUNCH_JSON" \
        "$BUNDLE_ID"; then
        break
    fi
    echo "[duo] Attempt $attempt failed; retrying in 2s..."
    sleep 2
done

read -r PID CORE_ID < <(python3 - "$LAUNCH_JSON" "$UDID" <<'PYCODE'
import json
import sys
with open(sys.argv[1]) as launch_file:
    d = json.load(launch_file)
pid = d['result']['process']['processIdentifier']
cid = d.get('result', {}).get('device', {}).get('identifier') or sys.argv[2]
print(pid, cid)
PYCODE
)

echo "[duo] Attaching LLDB to PID=$PID on CoreDevice=$CORE_ID..."
sleep 1
PYTHONPATH="$ROOT/scripts" xcrun lldb -b \
    -o "script lldb.debugger.SetAsync(False)" \
    -o "device select $CORE_ID" \
    -o "device process attach -p $PID" \
    -o "command script import $ROOT/scripts/_lldb_txm_jit.py" \
    -o "txm_jit_run"
