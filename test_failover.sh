#!/bin/bash
# Primary crash: client times out on h2, then asks h3 directly.
. "$(dirname "$0")/common.sh"
echo "== test_failover"
start_secondary; start_primary; sleep 0.3
out=$(client -t 300 wikipedia.org 2>/dev/null)
check "healthy: lookup works via primary" "$out" "Served by: primary"
kill $PRI_PID; wait $PRI_PID 2>/dev/null; sleep 0.2
out=$(client -t 200 -r 3 wikipedia.org 2>"$LOGDIR/client.err")
check "primary down: SUCCESS"             "$out" "Result: SUCCESS"
check "primary down: served by secondary" "$out" "Served by: secondary"
check "client logged failover"            "$(cat $LOGDIR/client.err)" "failing over to secondary"
out=$(client -t 200 -r 3 google.com 2>/dev/null)
check "replicated record served by secondary alone" "$out" "IP Address: 142.250.80.46"
summary
