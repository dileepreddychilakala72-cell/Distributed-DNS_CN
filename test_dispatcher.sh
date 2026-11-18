#!/bin/bash
# Round-robin distribution + health checks (down detection and recovery).
. "$(dirname "$0")/common.sh"
echo "== test_dispatcher"
start_secondary; start_primary; sleep 0.3
"$BIN/dispatcher" -p 9000 -b 127.0.0.1 -i 200 -m 3 $P_IP:$PORT $S_IP:$PORT >"$LOGDIR/disp.log" 2>&1 &
DPID=$!; PIDS+=($DPID); sleep 0.4
dclient() { "$BIN/client" -d 127.0.0.1:9000 -p $P_IP:$PORT -s $S_IP:$PORT -t 300 "$@"; }
for d in google.com example.org github.com wikipedia.org google.com pes.edu; do
    dclient $d 2>/dev/null | grep -q "Result: SUCCESS" || echo "  lookup $d failed"
done
np=$(grep -c "DISPATCH.*-> $P_IP" "$LOGDIR/disp.log"); ns=$(grep -c "DISPATCH.*-> $S_IP" "$LOGDIR/disp.log")
echo "  distribution: primary=$np secondary=$ns"
[ "$np" -eq 3 ] && [ "$ns" -eq 3 ] && { echo "  PASS: 6 requests split 3/3 round-robin"; PASS=$((PASS+1)); } || { echo "  FAIL: uneven split"; FAIL=$((FAIL+1)); }
kill $PRI_PID; wait $PRI_PID 2>/dev/null; sleep 1.2
check "dispatcher marked primary DOWN" "$(cat $LOGDIR/disp.log)" "backend $P_IP:$PORT is DOWN"
before=$(grep -c "DISPATCH.*-> $P_IP" "$LOGDIR/disp.log")
ok=0; for d in google.com example.org github.com wikipedia.org; do
    dclient $d 2>/dev/null | grep -q "Result: SUCCESS" && ok=$((ok+1)); done
after=$(grep -c "DISPATCH.*-> $P_IP" "$LOGDIR/disp.log")
[ $ok -eq 4 ] && [ "$before" -eq "$after" ] && { echo "  PASS: all 4 served by secondary, none sent to dead primary"; PASS=$((PASS+1)); } || { echo "  FAIL: ok=$ok before=$before after=$after"; FAIL=$((FAIL+1)); }
start_primary; sleep 1
check "primary back in rotation after recovery" "$(cat $LOGDIR/disp.log)" "backend $P_IP:$PORT is UP"
summary
