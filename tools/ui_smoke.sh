#!/bin/sh
# Manual end-to-end UI smoke. Requires Chrome on macOS and a release build.
# This is a developer tool, not a runtime or build dependency.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=${1:-"$ROOT/build"}
CHROME=${CHROME:-"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"}
SECONDS_TO_RUN=${SECONDS_TO_RUN:-10}

if [ ! -x "$BUILD/blackbox_launcher" ]; then
  echo "missing $BUILD/blackbox_launcher" >&2
  exit 1
fi
if [ ! -x "$CHROME" ]; then
  echo "missing Chrome at $CHROME (set CHROME=...)" >&2
  exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/blackbox-ui-smoke.XXXXXX")
cleanup() {
  if [ "${PID:-}" != "" ]; then
    kill "$PID" 2>/dev/null || true
    wait "$PID" 2>/dev/null || true
  fi
  rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

"$BUILD/blackbox_launcher" --no-browser --run-seconds "$SECONDS_TO_RUN" \
  --dump-dir "$TMP/dumps" >"$TMP/launcher.log" 2>&1 &
PID=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do
  if grep -q '^http://127\.0\.0\.1:' "$TMP/launcher.log" 2>/dev/null; then
    break
  fi
  sleep 0.4
done
URL=$(sed -n '2p' "$TMP/launcher.log")
case "$URL" in
  http://127.0.0.1:*) ;;
  *) echo "launcher did not print a loopback URL" >&2; cat "$TMP/launcher.log" >&2; exit 1 ;;
esac

"$CHROME" --headless=new --disable-gpu --hide-scrollbars \
  --window-size=1600,1200 --virtual-time-budget=4000 \
  --screenshot="$TMP/ui.png" "$URL" >/dev/null 2>&1 || true
if [ ! -s "$TMP/ui.png" ]; then
  echo "Chrome produced no screenshot" >&2
  exit 1
fi
echo "UI smoke screenshot: $TMP/ui.png (temporary, cleaned on exit)"
ls -l "$TMP/ui.png"
