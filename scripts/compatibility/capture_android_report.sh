#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Capture Bachata S4 Android compatibility evidence for one GitHub release/device/driver.

Usage:
  scripts/compatibility/capture_android_report.sh CUSAxxxxx [options]

Options:
  --release-tag TAG       Tested GitHub release tag; defaults to v<installed versionName>
  --device-label LABEL    Public device selector label; defaults to manufacturer + model
  --driver-type TYPE      system, turnip, or custom
  --driver-name NAME      Selected Vulkan driver name
  --driver-version VER    Selected system/custom driver version
  --turnip-version VER    Exact selected Turnip version (required with driver-type turnip)
  --turnip-build BUILD    Optional Turnip build/revision label
  --turnip-source SOURCE  Optional source/repository label
  --delay SECONDS         Wait before first screenshot (default: 60)
  --count NUMBER          Number of screenshots (default: 1)
  --interval SECONDS      Delay between screenshots (default: 15)
  --output DIRECTORY      Evidence output directory
  --no-launch             Do not start DirectLaunchActivity
  --keep-running          Do not force-stop the app before pulling logs
  -h, --help              Show this help

Environment:
  ADB=/path/to/adb        adb executable (default: adb)
  SERIAL=device-id        exact target serial; otherwise the only connected device

The ADB serial is stored only in the private capture.json work file and must never be
published. DirectLaunchActivity is debug-only; install a compatible debug APK first.
USAGE
}

GAME_ID="${1:-}"
if [[ -z "$GAME_ID" || "$GAME_ID" == -* ]]; then usage >&2; exit 2; fi
shift
GAME_ID="${GAME_ID^^}"
[[ "$GAME_ID" =~ ^CUSA[0-9]{5}$ ]] || { echo "error: game ID must look like CUSA00900" >&2; exit 2; }

DELAY=60
COUNT=1
INTERVAL=15
OUTPUT=""
DO_LAUNCH=1
KEEP_RUNNING=0
RELEASE_TAG=""
DEVICE_LABEL=""
DRIVER_TYPE=""
DRIVER_NAME=""
DRIVER_VERSION=""
TURNIP_VERSION=""
TURNIP_BUILD=""
TURNIP_SOURCE=""
while (($#)); do
  case "$1" in
    --release-tag) RELEASE_TAG="${2:?missing value for --release-tag}"; shift 2 ;;
    --device-label) DEVICE_LABEL="${2:?missing value for --device-label}"; shift 2 ;;
    --driver-type) DRIVER_TYPE="${2:?missing value for --driver-type}"; shift 2 ;;
    --driver-name) DRIVER_NAME="${2:?missing value for --driver-name}"; shift 2 ;;
    --driver-version) DRIVER_VERSION="${2:?missing value for --driver-version}"; shift 2 ;;
    --turnip-version) TURNIP_VERSION="${2:?missing value for --turnip-version}"; shift 2 ;;
    --turnip-build) TURNIP_BUILD="${2:?missing value for --turnip-build}"; shift 2 ;;
    --turnip-source) TURNIP_SOURCE="${2:?missing value for --turnip-source}"; shift 2 ;;
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
(( COUNT >= 1 && COUNT <= 3 )) || { echo "error: --count must be between 1 and 3" >&2; exit 2; }
if [[ -n "$DRIVER_TYPE" && ! "$DRIVER_TYPE" =~ ^(system|turnip|custom)$ ]]; then
  echo "error: --driver-type must be system, turnip, or custom" >&2; exit 2
fi
if [[ "$DRIVER_TYPE" == "turnip" && -z "$TURNIP_VERSION" ]]; then
  echo "error: --turnip-version is required with --driver-type turnip" >&2; exit 2
fi

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
  echo "error: adb executable not found: $ADB" >&2; exit 1
fi

mapfile -t CONNECTED < <("$ADB" devices | awk 'NR>1 && $2=="device" {print $1}')
if [[ -z "${SERIAL:-}" ]]; then
  if (( ${#CONNECTED[@]} == 0 )); then
    echo "error: no authorized Android device found" >&2; "$ADB" devices -l >&2 || true; exit 1
  elif (( ${#CONNECTED[@]} > 1 )); then
    echo "error: multiple devices are attached; export SERIAL to select one explicitly" >&2
    "$ADB" devices -l >&2 || true
    exit 1
  fi
  SERIAL="${CONNECTED[0]}"
fi
export ANDROID_SERIAL="$SERIAL"
ADB_CMD=("$ADB" -s "$SERIAL")
"${ADB_CMD[@]}" get-state >/dev/null

echo "Target selected: $SERIAL"
echo "Output: $OUTPUT"

prop() { "${ADB_CMD[@]}" shell getprop "$1" 2>/dev/null | tr -d '\r' | head -n 1; }
MANUFACTURER="$(prop ro.product.manufacturer)"
MODEL="$(prop ro.product.model)"
ANDROID_VERSION="$(prop ro.build.version.release)"
SOC="$(prop ro.soc.model)"; [[ -n "$SOC" ]] || SOC="$(prop ro.board.platform)"
GPU="$("${ADB_CMD[@]}" shell dumpsys SurfaceFlinger 2>/dev/null | tr -d '\r' | sed -nE 's/.*GLES:[[:space:]]*[^,]*,[[:space:]]*([^,]+).*/\1/p' | head -n 1 || true)"
[[ -n "$GPU" ]] || GPU="$(prop ro.hardware.egl)"
RAM_KB="$("${ADB_CMD[@]}" shell cat /proc/meminfo 2>/dev/null | tr -d '\r' | awk '/MemTotal:/ {print $2; exit}')"
RAM_GB="$(awk -v kb="${RAM_KB:-0}" 'BEGIN { if (kb > 0) printf "%.1f", kb/1024/1024; else print "" }')"
[[ -n "$DEVICE_LABEL" ]] || DEVICE_LABEL="${MANUFACTURER} ${MODEL}"

MANUFACTURER="$MANUFACTURER" MODEL="$MODEL" SOC="$SOC" GPU="$GPU" DEVICE_LABEL="$DEVICE_LABEL" \
ANDROID_VERSION="$ANDROID_VERSION" RAM_GB="$RAM_GB" python3 - "$OUTPUT/device.json" <<'PY'
import json, os, sys
value = {
    "label": os.environ.get("DEVICE_LABEL") or "Unknown device",
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
  echo "Interact with the game now. First screenshot in ${DELAY}s."
else
  echo "Launch skipped. Capture begins in ${DELAY}s."
fi

sleep "$DELAY"
SCREENSHOTS=()
for ((index=1; index<=COUNT; index++)); do
  shot="$OUTPUT/screenshots/${GAME_ID,,}-${STAMP}-$(printf '%02d' "$index").png"
  "${ADB_CMD[@]}" exec-out screencap -p > "$shot"
  [[ -s "$shot" ]] || { echo "error: screenshot capture produced an empty file" >&2; exit 1; }
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
SOURCE_VERSION="$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || true)"
PACKAGE_VERSION="$("${ADB_CMD[@]}" shell dumpsys package "$PACKAGE" 2>/dev/null | tr -d '\r' | sed -nE 's/^[[:space:]]*versionName=//p' | head -n 1 || true)"
[[ -n "$RELEASE_TAG" ]] || { [[ -n "$PACKAGE_VERSION" ]] && RELEASE_TAG="v${PACKAGE_VERSION#v}"; }

ROOT="$ROOT" OUTPUT="$OUTPUT" GAME_ID="$GAME_ID" SERIAL="$SERIAL" STAMP="$STAMP" \
COMMIT="$COMMIT" SOURCE_VERSION="$SOURCE_VERSION" PACKAGE_VERSION="$PACKAGE_VERSION" RELEASE_TAG="$RELEASE_TAG" \
DEVICE_LABEL="$DEVICE_LABEL" DRIVER_TYPE="$DRIVER_TYPE" DRIVER_NAME="$DRIVER_NAME" DRIVER_VERSION="$DRIVER_VERSION" \
TURNIP_VERSION="$TURNIP_VERSION" TURNIP_BUILD="$TURNIP_BUILD" TURNIP_SOURCE="$TURNIP_SOURCE" \
python3 - "${SCREENSHOTS[@]}" <<'PY'
import json, os, sys
out = os.environ["OUTPUT"]
metadata = {
    "gameId": os.environ["GAME_ID"],
    "deviceSerial": os.environ["SERIAL"],
    "deviceLabel": os.environ.get("DEVICE_LABEL", ""),
    "capturedAt": os.environ["STAMP"],
    "releaseTag": os.environ.get("RELEASE_TAG", ""),
    "commit": os.environ.get("COMMIT", ""),
    "sourceVersion": os.environ.get("SOURCE_VERSION", ""),
    "installedVersion": os.environ.get("PACKAGE_VERSION", ""),
    "driver": {
        "driverType": os.environ.get("DRIVER_TYPE", ""),
        "driver": os.environ.get("DRIVER_NAME", ""),
        "driverVersion": os.environ.get("DRIVER_VERSION", ""),
        "turnipVersion": os.environ.get("TURNIP_VERSION", ""),
        "turnipBuild": os.environ.get("TURNIP_BUILD", ""),
        "turnipSource": os.environ.get("TURNIP_SOURCE", ""),
    },
    "deviceJson": os.path.join(out, "device.json"),
    "screenshots": sys.argv[1:],
    "logcat": os.path.join(out, "logcat.txt"),
    "sessionLogs": os.path.join(out, "session-logs"),
}
metadata["driver"] = {k: v for k, v in metadata["driver"].items() if v}
with open(os.path.join(out, "capture.json"), "w", encoding="utf-8") as handle:
    json.dump(metadata, handle, indent=2)
    handle.write("\n")
PY

echo
echo "Capture complete. Review screenshots/logs and verify the release, selected device, and selected driver."
echo "Evidence directory: $OUTPUT"
echo "Release candidate: ${RELEASE_TAG:-unknown}"
echo "Selected device: $DEVICE_LABEL"
if [[ "$DRIVER_TYPE" == "turnip" ]]; then echo "Selected driver: ${DRIVER_NAME:-Turnip} ${TURNIP_VERSION}"; else echo "Selected driver: ${DRIVER_NAME:-not recorded}"; fi
echo "Next: add the report with --release-tag, --device-json, --driver-type, driver details, screenshots, and logs."
