#!/bin/bash
# Failure combinations: both down, secondary down while primary up.
. "$(dirname "$0")/common.sh"
echo "== test_server_failure"
out=$(client -t 100 -r 3 google.com 2>/dev/null); rc=$?
check "both down -> SERVER_UNAVAILABLE"   "$out" "Result: SERVER_UNAVAILABLE"
[ $rc -eq 3 ] && { echo "  PASS: exit code 3"; PASS=$((PASS+1)); } || { echo "  FAIL: exit code $rc"; FAIL=$((FAIL+1)); }
out=$(client -t 100 -r 20 -D 600 google.com 2>/dev/null)
check "tight overall deadline -> TIMEOUT" "$out" "Result: TIMEOUT"
start_primary; sleep 0.3          # secondary NOT running
out=$(client -t 300 google.com 2>/dev/null)
check "secondary down, local record still OK" "$out" "Result: SUCCESS"
out=$(client -t 300 -r 2 wikipedia.org 2>/dev/null)
check "secondary down, forward fails -> not a false NOTFOUND" "$out" "Result: SERVER_UNAVAILABLE"
summary
