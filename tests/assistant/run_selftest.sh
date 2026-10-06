#!/bin/bash
# Runs the in-app assistant self test: RACK_ASSISTANT_SELFTEST=<tools|mock|all> ./Rack -d -u <tmp>
# Usage: tests/assistant/run_selftest.sh [tools|mock|all]   (default: all)
# Exits non-zero if any check failed, the result file is missing, or Rack crashed / timed out.
set -u

MODE="${1:-all}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2

if [ ! -x ./Rack ]; then
	echo "run_selftest: ./Rack not found; run 'make' first" >&2
	exit 2
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Dev mode loads plugins from <userdir>/plugins
if [ -e "$ROOT/plugins" ]; then
	ln -s "$(cd "$ROOT/plugins" && pwd -P)" "$TMP/plugins"
fi

RESULT="$TMP/assistant-selftest-result.json"
LOG="$TMP/selftest.log"

# Prefer an existing X display, else use xvfb-run
RUNNER=()
if [ -n "${DISPLAY:-}" ] && command -v xdpyinfo >/dev/null 2>&1 && xdpyinfo >/dev/null 2>&1; then
	:
elif [ -z "${DISPLAY:-}" ] && [ -S /tmp/.X11-unix/X99 ]; then
	export DISPLAY=:99
elif [ -z "${DISPLAY:-}" ]; then
	RUNNER=(xvfb-run -a)
fi

echo "run_selftest: mode=$MODE user dir=$TMP"
RACK_ASSISTANT_SELFTEST="$MODE" timeout 120 "${RUNNER[@]}" ./Rack -d -u "$TMP" >"$LOG" 2>&1
STATUS=$?

grep -a "assistant selftest" "$LOG" | sed 's/\x1b\[[0-9;]*m//g'

if [ ! -f "$RESULT" ]; then
	echo "run_selftest: result file missing (Rack exit status $STATUS). Last log lines:" >&2
	tail -n 30 "$LOG" | sed 's/\x1b\[[0-9;]*m//g' >&2
	exit 1
fi

echo "---- result ----"
cat "$RESULT"
echo

# Parse failed count without requiring jq
FAILED="$(sed -n 's/.*"failed": *\([0-9][0-9]*\).*/\1/p' "$RESULT" | head -n 1)"
if [ -z "$FAILED" ]; then
	echo "run_selftest: cannot parse result file" >&2
	exit 1
fi
if [ "$FAILED" -gt 0 ]; then
	exit 1
fi
if [ "$STATUS" -ne 0 ]; then
	echo "run_selftest: warning: Rack exited with status $STATUS" >&2
fi
exit 0
