#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Capture Bachata S4 Android compatibility evidence.

Usage:
  scripts/compatibility/capture_android_report.sh CUSAxxxxx [options]

Options:
  --delay SECONDS       Wait before the first screenshot (default: 60)
  --count NUMBER        Number of screenshots (default: 1)
  --interval SECONDS    Delay between screenshots (default: 15)
  --output DIRECTORY    Evidence output directory
  --no-launch           Do not start DirectLaunchActivity
  --keep-running        Do not force-stop the app before pulling logs
  -h, --help            Show this help

Environment:
  ADB=/path/to/adb      adb executable (default: adb)
  SERIAL=device-id      target serial; otherwise the first connected device

DirectLaunchActivity is debug-only. Install a debug APK before using launch mode.
USAGE
}

GAME_ID="${1:-}"
if [[ -z "$GAME_ID" || "$GAME_ID" == -* ]]; then
  usage >&2
  exit 2
fi
shift
GAME_ID="${GAME_ID^^}"
if [[ ! "$GAME_ID" =~ ^CUSA[0-9]{5}$ ]]; then
  echo "error: game ID must look like CUSA00900" >&2
  exit 2
fi

DELAY=60
COUNT=1
INTERVAL=15
OUTPUT=""
DO_LAUNCH=1
KEEP_RUNNING=0
while (($#)); do
  case "$1" in
    --delay) DELAY="${2:?missing value for --delay}"; shift 2 ;;
    --count) COUNT="${2:?missing value for --count}"; shift 2 ;;
    --interval) INTERVAL="${2:?missing value for --interval}"; shift 2 ;;
    --output) OUTPUT="${2:?missing value for --output}"; shift 2 ;;
    --no-launch) DO_LAUNCH=0; shift ;;
    --keep-running) KEEP_RUNNING=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "error: unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

for value in "$DELAY" "$COUNT" "$INTERVAL"; do
  [[ "$value" =~ ^[0-9]+$ ]] || { echo "error: timing/count values must be non-negative integers" >&2; exit 2; }
done
(( COUNT >= 1 )) || { echo "error: --count must be at least 1" >&2; exit 2; }

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
ADB="${ADB:-adb}"
PACKAGE="com.bachatas4.android"
STAMP="$(date -u +%Y%m%d-%H%M%S)"
GIT_DIR="$(git -C "$ROOT" rev-parse --absolute-git-dir 2>/dev/null || true)"
WORK_BASE="${GIT_DIR:-/tmp}/compatibility-work"
OUTPUT="${OUTPUT:-$WORK_BASE/${GAME_ID,,}/$STAMP}"
mkdir -p "$OUTPUT/screenshots" "$OUTPUT/session-logs"
OUTPUT="$(cd "$OUTPUT" && pwd)"

if ! command -v "$ADB" >/dev/null 2>&1 && [[ ! -x "$ADB" ]]; then
  echo "error: adb executable not found: $ADB" >&2
  exit 1
fi

if [[ -z "${SERIAL:-}" ]]; then
  SERIAL="$($ADB devices | awk 'NR>1 && $2=="device" {print $1; exit}')"
fi
if [[ -z "${SERIAL:-}" ]]; then
  echo "error: no authorized Android device found" >&2
  "$ADB" devices -l >&2 || true
  exit 1
fi
export ANDROID_SERIAL="$SERIAL"
ADB_CMD=("$ADB" -s "$SERIAL")
"${ADB_CMD[@]}" get-state >/dev/null

echo "Target: $SERIAL"
echo "Output: $OUTPUT"

prop() {
  "${ADB_CMD[@]}" shell getprop "$1" 2>/dev/null | tr -d '\r' | head -n 1
}

MANUFACTURER="$(prop ro.product.manufacturer)"
MODEL="$(prop ro.product.model)"
ANDROID_VERSION="$(prop ro.build.version.release)"
SOC="$(prop ro.soc.model)"
[[ -n "$SOC" ]] || SOC="$(prop ro.board.platform)"
GPU="$("${ADB_CMD[@]}" shell dumpsys SurfaceFlinger 2>/dev/null | tr -d '\r' | sed -nE 's/.*GLES:[[:space:]]*[^,]*,[[:space:]]*([^,]+).*/\1/p' | head -n 1 || true)"
[[ -n "$GPU" ]] || GPU="$(prop ro.hardware.egl)"
RAM_KB="$("${ADB_CMD[@]}" shell cat /proc/meminfo 2>/dev/null | tr -d '\r' | awk '/MemTotal:/ {print $2; exit}')"
RAM_GB="$(awk -v kb="${RAM_KB:-0}" 'BEGIN { if (kb > 0) printf "%.1f", kb/1024/1024; else print "" }')"

MANUFACTURER="$MANUFACTURER" MODEL="$MODEL" SOC="$SOC" GPU="$GPU" \
ANDROID_VERSION="$ANDROID_VERSION" RAM_GB="$RAM_GB" python3 - "$OUTPUT/device.json" <<'PY'
import json, os, sys
value = {
    "manufacturer": os.environ.get("MANUFACTURER") or "Unknown",
    "model": os.environ.get("MODEL") or "Unknown",
    "soc": os.environ.get("SOC") or "Unknown",
    "gpu": os.environ.get("GPU") or "Unknown",
    "androidVersion": os.environ.get("ANDROID_VERSION") or "Unknown",
}
ram = os.environ.get("RAM_GB", "")
if ram:
    value["ramGb"] = float(ram)
with open(sys.argv[1], "w", encoding="utf-8") as handle:
    json.dump(value, handle, indent=2)
    handle.write("\n")
PY

"${ADB_CMD[@]}" logcat -c || true

if (( DO_LAUNCH )); then
  echo "Launching $GAME_ID with DirectLaunchActivity…"
  "${ADB_CMD[@]}" shell am start -W -n "$PACKAGE/.DirectLaunchActivity" --es game_id "$GAME_ID" | tee "$OUTPUT/am-start.txt"
  echo "Interact with the game on the device now. First screenshot in ${DELAY}s."
else
  echo "Launch skipped. Capture begins in ${DELAY}s."
fi

sleep "$DELAY"
SCREENSHOTS=()
for ((index=1; index<=COUNT; index++)); do
  shot="$OUTPUT/screenshots/${GAME_ID,,}-${STAMP}-$(printf '%02d' "$index").png"
  "${ADB_CMD[@]}" exec-out screencap -p > "$shot"
  if [[ ! -s "$shot" ]]; then
    echo "error: screenshot capture produced an empty file" >&2
    exit 1
  fi
  SCREENSHOTS+=("$shot")
  echo "Captured: $shot"
  if (( index < COUNT )); then sleep "$INTERVAL"; fi
done

"${ADB_CMD[@]}" shell uiautomator dump /sdcard/bachata-window.xml >/dev/null 2>&1 || true
"${ADB_CMD[@]}" pull /sdcard/bachata-window.xml "$OUTPUT/window.xml" >/dev/null 2>&1 || true
"${ADB_CMD[@]}" shell rm -f /sdcard/bachata-window.xml >/dev/null 2>&1 || true

if (( ! KEEP_RUNNING )); then
  echo "Stopping app to flush session logs…"
  "${ADB_CMD[@]}" shell am force-stop "$PACKAGE" || true
  sleep 2
fi

"${ADB_CMD[@]}" logcat -d -v threadtime > "$OUTPUT/logcat.txt" || true

echo "Pulling Bachata session logs from the selected device…"
APP_LOG_ROOT="files/logs"
SESSIONS="$("${ADB_CMD[@]}" exec-out run-as "$PACKAGE" sh -c "cd '$APP_LOG_ROOT' 2>/dev/null && ls -1" 2>/dev/null | tr -d '\r' | sed '/^$/d' | sort || true)"
MATCHED_SESSION="$(printf '%s\n' "$SESSIONS" | grep -F "$GAME_ID" | tail -n 1 || true)"
if [[ -z "$MATCHED_SESSION" ]]; then
  echo "warning: no app-private session folder matched $GAME_ID; use logcat.txt as evidence" >&2
else
  SESSION_DEST="$OUTPUT/session-logs/$MATCHED_SESSION"
  mkdir -p "$SESSION_DEST"
  PULLED_ANY=0
  for file in application.log shadps4.log shadps4-internal.log; do
    if "${ADB_CMD[@]}" exec-out run-as "$PACKAGE" sh -c "test -f '$APP_LOG_ROOT/$MATCHED_SESSION/$file'" >/dev/null 2>&1; then
      "${ADB_CMD[@]}" exec-out run-as "$PACKAGE" cat "$APP_LOG_ROOT/$MATCHED_SESSION/$file" > "$SESSION_DEST/$file"
      PULLED_ANY=1
      echo "Pulled: $SESSION_DEST/$file"
    fi
  done
  if (( ! PULLED_ANY )); then
    rmdir "$SESSION_DEST" 2>/dev/null || true
    echo "warning: matched session $MATCHED_SESSION contained no known log files; use logcat.txt as evidence" >&2
  fi
fi

COMMIT="$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || true)"
VERSION="$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || true)"
PACKAGE_VERSION="$("${ADB_CMD[@]}" shell dumpsys package "$PACKAGE" 2>/dev/null | tr -d '\r' | sed -nE 's/^[[:space:]]*versionName=//p' | head -n 1 || true)"

ROOT="$ROOT" OUTPUT="$OUTPUT" GAME_ID="$GAME_ID" SERIAL="$SERIAL" STAMP="$STAMP" \
COMMIT="$COMMIT" VERSION="$VERSION" PACKAGE_VERSION="$PACKAGE_VERSION" \
python3 - "${SCREENSHOTS[@]}" <<'PY'
import json, os, sys
out = os.environ["OUTPUT"]
metadata = {
    "gameId": os.environ["GAME_ID"],
    "deviceSerial": os.environ["SERIAL"],
    "capturedAt": os.environ["STAMP"],
    "commit": os.environ.get("COMMIT", ""),
    "emulatorVersion": os.environ.get("VERSION", ""),
    "installedVersion": os.environ.get("PACKAGE_VERSION", ""),
    "deviceJson": os.path.join(out, "device.json"),
    "screenshots": sys.argv[1:],
    "logcat": os.path.join(out, "logcat.txt"),
    "sessionLogs": os.path.join(out, "session-logs"),
}
with open(os.path.join(out, "capture.json"), "w", encoding="utf-8") as handle:
    json.dump(metadata, handle, indent=2)
    handle.write("\n")
PY

echo
echo "Capture complete. Review the screenshots and logs before classifying the game."
echo "Evidence directory: $OUTPUT"
echo "Next: run scripts/compatibility/add_report.py with --device-json '$OUTPUT/device.json',"
echo "      one or more --screenshot paths, and an unmodified pulled session log or logcat.txt."
