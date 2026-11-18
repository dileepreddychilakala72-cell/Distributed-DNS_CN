#!/bin/bash
# Simulated loss (-l 30 = 30% of queries and 30% of responses dropped by the
# servers).  Under Mininet use netem instead (see README).
. "$(dirname "$0")/common.sh"
echo "== test_packet_loss"
start_secondary -l 30; start_primary -l 30; sleep 0.3
ok=0; retried=0; N=20
for i in $(seq $N); do
    out=$(client -t 50 -r 12 -D 20000 google.com 2>"$LOGDIR/c.err")
    grep -q "Result: SUCCESS" <<<"$out" && ok=$((ok+1))
    grep -q "attempt 2" "$LOGDIR/c.err" && retried=$((retried+1))
done
echo "  $ok/$N lookups succeeded, $retried needed at least one retransmission"
[ $ok -ge $((N-1)) ] && { echo "  PASS: retransmission recovers from loss"; PASS=$((PASS+1)); } || { echo "  FAIL: too many failures"; FAIL=$((FAIL+1)); }
[ $retried -ge 1 ] && { echo "  PASS: retries actually occurred"; PASS=$((PASS+1)); } || { echo "  FAIL: no retries seen (loss not applied?)"; FAIL=$((FAIL+1)); }
check "servers dropped packets"  "$(cat $LOGDIR/primary.log)" "SIM-LOSS"
summary
