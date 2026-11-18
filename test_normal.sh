#!/bin/bash
# Core DNS: primary lookup, forwarded secondary lookup, unknown domain.
. "$(dirname "$0")/common.sh"
echo "== test_normal"
start_secondary; start_primary; sleep 0.3
out=$(client -t 300 google.com 2>/dev/null)
check "primary lookup returns IP"         "$out" "IP Address: 142.250.80.46"
check "answered by primary"               "$out" "Served by: primary"
out=$(client -t 300 wikipedia.org 2>/dev/null)
check "secondary-only record resolved"    "$out" "IP Address: 208.80.154.224"
check "primary forwarded to secondary"    "$(cat $LOGDIR/primary.log)" "FORWARD to secondary"
check "secondary answered forwarded query" "$(cat $LOGDIR/secondary.log)" "wikipedia.org -> 208.80.154.224"
out=$(client -t 300 nosuch.invalid 2>/dev/null)
check "unknown domain -> NOTFOUND"        "$out" "Result: NOTFOUND"
summary
