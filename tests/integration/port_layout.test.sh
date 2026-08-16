#!/usr/bin/env bash
# Suite 10 — Port Layout & Startup Tests
# Usage: ANOA_BINARY=./build/anoa bash tests/integration/port_layout.test.sh
set -euo pipefail

BINARY="${ANOA_BINARY:-./build/anoa}"
PORT="${ANOA_PORT:-9222}"
WS_PORT=$((PORT + 2))
PASS=0
FAIL=0
PROC_PID=""

wait_for_port() {
  local port=$1 timeout=${2:-15}
  local start=$SECONDS
  while ! nc -z 127.0.0.1 "$port" 2>/dev/null; do
    [ $((SECONDS - start)) -ge "$timeout" ] && return 1
    sleep 0.2
  done
}

start_browser() {
  local p=$1; shift
  local extra_args=("$@")
  PROC_PID=""
  # "${extra_args[@]+...}" rather than "${extra_args[@]}": under set -u, bash
  # 3.2 — which is what macOS ships — treats an EMPTY array expansion as an
  # unbound variable and kills the command. Every PORT case then reported
  # "Browser failed to start", on a browser that had started fine a moment
  # earlier by hand. bash 5 on CI expands it happily, so the suite only ever
  # failed on a developer's machine.
  QPA_PLATFORM=offscreen "$BINARY" \
    --headless --no-sandbox "--port=$p" \
    ${extra_args[@]+"${extra_args[@]}"} &
  PROC_PID=$!
  if ! wait_for_port "$p" 15; then
    kill "$PROC_PID" 2>/dev/null || true
    return 1
  fi
}

stop_browser() {
  if [ -n "$PROC_PID" ]; then
    kill "$PROC_PID" 2>/dev/null || true
    wait "$PROC_PID" 2>/dev/null || true
    PROC_PID=""
  fi
}

assert_pass() { echo "  PASS  $1"; PASS=$((PASS + 1)); }
assert_fail() { echo "  FAIL  $1${2:+ — $2}"; FAIL=$((FAIL + 1)); }

# PORT-01: Default ports — HTTP on PORT and WS on PORT+2
echo "=== PORT-01: HTTP and WS ports are listening ==="
if start_browser "$PORT"; then
  HTTP_UP=false; WS_UP=false
  nc -z 127.0.0.1 "$PORT" 2>/dev/null && HTTP_UP=true
  nc -z 127.0.0.1 "$WS_PORT" 2>/dev/null && WS_UP=true
  if $HTTP_UP && $WS_UP; then
    assert_pass "PORT-01: HTTP port $PORT and WS port $WS_PORT are listening"
  else
    assert_fail "PORT-01: HTTP=$HTTP_UP WS=$WS_UP"
  fi
  stop_browser
else
  assert_fail "PORT-01: Browser failed to start"
fi

# PORT-02: Custom port — HTTP on 8800, WS on 8802
echo "=== PORT-02: Custom port layout ==="
CUSTOM_PORT=8800
if start_browser "$CUSTOM_PORT"; then
  HTTP_UP=false; WS_UP=false
  nc -z 127.0.0.1 "$CUSTOM_PORT" 2>/dev/null && HTTP_UP=true
  nc -z 127.0.0.1 $((CUSTOM_PORT + 2)) 2>/dev/null && WS_UP=true
  if $HTTP_UP && $WS_UP; then
    assert_pass "PORT-02: Custom port layout: HTTP=$CUSTOM_PORT WS=$((CUSTOM_PORT+2))"
  else
    assert_fail "PORT-02: HTTP=$HTTP_UP WS=$WS_UP on custom port $CUSTOM_PORT"
  fi
  # Cleanup — use the custom proc PID
  kill "$PROC_PID" 2>/dev/null || true
  wait "$PROC_PID" 2>/dev/null || true
  PROC_PID=""
else
  assert_fail "PORT-02: Browser failed to start on custom port $CUSTOM_PORT"
fi

# PORT-03: Port conflict detection — pre-bind port, then try to start binary
echo "=== PORT-03: Port conflict detection ==="
# Pre-bind port with netcat in listen mode.
#
# BSD netcat, which is what macOS ships, does not take a host with -l the way
# GNU netcat does, so the bind silently does not happen and anoa starts
# perfectly well on a port the test believes is taken. That reported a
# conflict-detection failure on a binary that was behaving correctly. The bind
# is verified rather than assumed, and the case says it skipped instead.
if [ "$(uname -s)" = "Darwin" ]; then
  # netcat binds 127.0.0.1 while anoa binds 0.0.0.0, and macOS lets both hold
  # the same port — the conflict this case exists to detect cannot be staged
  # there. Linux refuses the second bind, so CI still exercises it fully.
  echo "  SKIP  PORT-03: macOS allows 0.0.0.0 and 127.0.0.1 to share a port"
else
nc -l 127.0.0.1 "$PORT" &>/dev/null &
NC_PID=$!
sleep 0.5
TMPOUT=$(mktemp)
QPA_PLATFORM=offscreen "$BINARY" \
  --headless --no-sandbox "--port=$PORT" \
  >"$TMPOUT" 2>&1 &
SUBPID=$!
# Give it time to try to bind and fail
sleep 3
if kill -0 "$SUBPID" 2>/dev/null; then
  # Still running even with port conflict — unexpected
  kill "$SUBPID" 2>/dev/null || true
  wait "$SUBPID" 2>/dev/null || true
  # The HTTP server may have failed silently; check /json health
  # This is a soft assertion since Qt doesn't always hard-exit on bind failure
  assert_fail "PORT-03: Binary continued running despite port conflict (may be soft error)"
else
  # `|| EXIT_CODE=$?` keeps set -e from aborting the script on the child's
  # (expected) non-zero exit status.
  EXIT_CODE=0
  wait "$SUBPID" || EXIT_CODE=$?
  if [ "$EXIT_CODE" -ne 0 ]; then
    assert_pass "PORT-03: Binary exited non-zero when port was already in use"
  else
    assert_fail "PORT-03: Binary exited 0 despite port conflict"
  fi
fi
kill "$NC_PID" 2>/dev/null || true
wait "$NC_PID" 2>/dev/null || true
rm -f "$TMPOUT"
fi

# PORT-04: /json/list non-empty after startup (startup navigation)
echo "=== PORT-04: /json/list returns at least one target ==="
if start_browser "$PORT"; then
  BODY=$(curl -sf "http://localhost:$PORT/json/list" 2>/dev/null || echo "[]")
  COUNT=$(echo "$BODY" | python3 -c "import sys,json; print(len(json.load(sys.stdin)))" 2>/dev/null || echo "0")
  if [ "$COUNT" -ge 1 ]; then
    assert_pass "PORT-04: /json/list has $COUNT target(s)"
  else
    assert_fail "PORT-04: /json/list returned empty array or error"
  fi
  stop_browser
else
  assert_fail "PORT-04: Browser failed to start"
fi

# PORT-05: Headless mode on CI (no $DISPLAY)
echo "=== PORT-05: Headless mode works without display ==="
SAVED_DISPLAY="${DISPLAY:-}"
export DISPLAY=""
if start_browser "$PORT"; then
  HTTP_STATUS=$(curl -s -o /dev/null -w "%{http_code}" "http://localhost:$PORT/json/version" || echo "000")
  if [ "$HTTP_STATUS" = "200" ]; then
    assert_pass "PORT-05: Headless mode works without DISPLAY"
  else
    assert_fail "PORT-05: HTTP status $HTTP_STATUS without DISPLAY"
  fi
  stop_browser
else
  assert_fail "PORT-05: Browser failed to start without DISPLAY"
fi
[ -n "$SAVED_DISPLAY" ] && export DISPLAY="$SAVED_DISPLAY" || unset DISPLAY

# PORT-07: --no-sandbox flag accepted without crash
echo "=== PORT-07: --no-sandbox flag accepted ==="
if start_browser "$PORT" "--no-sandbox"; then
  HTTP_STATUS=$(curl -s -o /dev/null -w "%{http_code}" "http://localhost:$PORT/json/version" || echo "000")
  if [ "$HTTP_STATUS" = "200" ]; then
    assert_pass "PORT-07: --no-sandbox accepted, HTTP still responds"
  else
    assert_fail "PORT-07: HTTP status $HTTP_STATUS with --no-sandbox"
  fi
  stop_browser
else
  assert_fail "PORT-07: Browser failed to start with --no-sandbox"
fi

# PORT-INVALID-LOW: --port 0 exits with code 1 (CFG-03)
echo "=== PORT-INVALID-LOW: --port 0 exits with code 1 ==="
TMPOUT=$(mktemp)
QPA_PLATFORM=offscreen "$BINARY" --headless --no-sandbox --port 0 >"$TMPOUT" 2>&1 || EXIT_CODE=$?
EXIT_CODE=${EXIT_CODE:-0}
if [ "$EXIT_CODE" -eq 1 ]; then
  assert_pass "PORT-INVALID-LOW: --port 0 exits 1 (CFG-03)"
else
  assert_fail "PORT-INVALID-LOW: Expected exit 1, got $EXIT_CODE (CFG-03)"
fi
rm -f "$TMPOUT"

# PORT-INVALID-HIGH: --port 99999 exits with code 1 (CFG-04)
echo "=== PORT-INVALID-HIGH: --port 99999 exits with code 1 ==="
TMPOUT=$(mktemp)
QPA_PLATFORM=offscreen "$BINARY" --headless --no-sandbox --port 99999 >"$TMPOUT" 2>&1 || EXIT_CODE=$?
EXIT_CODE=${EXIT_CODE:-0}
if [ "$EXIT_CODE" -eq 1 ]; then
  assert_pass "PORT-INVALID-HIGH: --port 99999 exits 1 (CFG-04)"
else
  assert_fail "PORT-INVALID-HIGH: Expected exit 1, got $EXIT_CODE (CFG-04)"
fi
rm -f "$TMPOUT"

# ── Mode dispatch (TERM-MODE-01..05) ────────────────────────────────────────
#
# `terminal` is a bare positional word consumed by a raw-argv pre-scan in
# main.cpp, before any application object exists — it is what selects
# QCoreApplication over QApplication, so it cannot be a QCommandLineParser
# positional. None of that is reachable from the pty suites: they all get past
# dispatch before their first assertion, so a regression in the pre-scan would
# show up there as "everything times out" and nowhere as a diagnosis.
#
# These cases live here rather than in a vitest file because none of them needs
# a pty — they are exit codes and one-line messages, which is what this suite
# already owns.

run_cli() {
  # Run the binary, capture stdout+stderr into $CLI_OUT and the status into
  # $CLI_CODE without tripping `set -e`. stdin is /dev/null on purpose: it is
  # what makes TERM-MODE-01 a pipe rather than whatever this script inherited.
  CLI_OUT=$("$@" </dev/null 2>&1) && CLI_CODE=0 || CLI_CODE=$?
}

echo "=== TERM-MODE-01: terminal mode refuses a non-tty ==="
run_cli "$BINARY" terminal
if [ "$CLI_CODE" -eq 1 ] && grep -q "stdin/stdout must be a terminal" <<<"$CLI_OUT"; then
  assert_pass "TERM-MODE-01: terminal over a pipe exits 1 with the documented message"
else
  assert_fail "TERM-MODE-01: exit $CLI_CODE, output: $(head -c 200 <<<"$CLI_OUT")"
fi

echo "=== TERM-MODE-02: terminal --help does not echo the subcommand back ==="
run_cli "$BINARY" terminal --help
USAGE_LINE=$(head -1 <<<"$CLI_OUT")
# The pre-scan shifts `terminal` out of argv so QCommandLineParser never sees
# it; left in, it would come back as a positional argument nobody can pass
# twice.
#
# This used to assert that the first line ended in "[options]", which was
# QCommandLineParser's own usage line. --help prints the grouped help now — the
# same text as `anoa help`, because a flag list that cannot mention a
# subcommand left users with no idea `click` existed — so there is no
# auto-generated usage line to end in anything. What still has to hold is that
# the subcommand does not come back as an argument, and that asking for help
# succeeds.
if [ "$CLI_CODE" -eq 0 ] \
   && grep -q "a browser you drive from the command line" <<<"$CLI_OUT" \
   && ! grep -qE "^Usage:.*\bterminal\b" <<<"$CLI_OUT"; then
  assert_pass "TERM-MODE-02a: terminal --help succeeds without echoing the subcommand"
else
  assert_fail "TERM-MODE-02a: exit $CLI_CODE, first line: $USAGE_LINE"
fi

# Both modes share one QCommandLineParser, so every flag is listed in both
# --help outputs. That is the price of never overloading a flag by mode, and it
# only stays true if something checks.
TERM_FLAGS=(--term-host --term-port --term-token --fps --gfx --cdp)
MISSING=""
for flag in "${TERM_FLAGS[@]}"; do
  grep -q -- "$flag" <<<"$CLI_OUT" || MISSING="$MISSING $flag"
done
if [ -z "$MISSING" ]; then
  assert_pass "TERM-MODE-02b: terminal --help lists all ${#TERM_FLAGS[@]} terminal options"
else
  assert_fail "TERM-MODE-02b: missing from terminal --help:$MISSING"
fi

echo "=== TERM-MODE-03: browser --help lists the same terminal options ==="
# QT_QPA_PLATFORM, not the QPA_PLATFORM this file uses elsewhere: browser mode
# constructs a QApplication before parseArgs runs, and without a display or the
# real Qt variable that aborts before --help is ever printed.
run_cli env QT_QPA_PLATFORM=offscreen "$BINARY" --help
MISSING=""
for flag in "${TERM_FLAGS[@]}"; do
  grep -q -- "$flag" <<<"$CLI_OUT" || MISSING="$MISSING $flag"
done
if [ "$CLI_CODE" -eq 0 ] && [ -z "$MISSING" ]; then
  assert_pass "TERM-MODE-03: browser --help lists the terminal options too"
else
  assert_fail "TERM-MODE-03: exit $CLI_CODE, missing:$MISSING"
fi

echo "=== TERM-MODE-04: --cdp wss:// is rejected by the shipped binary ==="
# TERM-CFG-09 covers this through the parse_args test harness; this covers the
# binary a user actually runs, which is a different code path into the same
# validation only as long as nobody moves the check.
run_cli "$BINARY" terminal --cdp wss://example.invalid/devtools/page/X
if [ "$CLI_CODE" -eq 1 ] && grep -q "wss://" <<<"$CLI_OUT"; then
  assert_pass "TERM-MODE-04: --cdp wss:// exits 1 naming the scheme"
else
  assert_fail "TERM-MODE-04: exit $CLI_CODE, output: $(head -c 200 <<<"$CLI_OUT")"
fi

echo "=== TERM-MODE-05: the accepted pre-scan limitation ==="
# Known and accepted: the pre-scan has no option-arity knowledge, so the word
# `terminal` is taken as the subcommand wherever it appears — including as the
# value of an option. `--profile terminal` therefore loses its value and the
# parser reports a missing one. This is a limitation with a test rather than a
# limitation someone rediscovers as a bug; if the pre-scan ever learns arity,
# this case is the one to delete.
run_cli env QT_QPA_PLATFORM=offscreen "$BINARY" --profile terminal
if [ "$CLI_CODE" -eq 1 ] && grep -qi "missing value" <<<"$CLI_OUT"; then
  assert_pass "TERM-MODE-05: a profile named 'terminal' is eaten by the pre-scan, as documented"
else
  assert_fail "TERM-MODE-05: exit $CLI_CODE, output: $(head -c 200 <<<"$CLI_OUT")"
fi

# Summary
echo ""
echo "Port Layout Tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
