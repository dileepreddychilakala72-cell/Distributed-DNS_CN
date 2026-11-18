# Shared helpers.  Tests run on loopback (127.0.0.2 = "h2", 127.0.0.3 = "h3")
# so they work without Mininet/root.  For Mininet see README.md.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/bin"; DATA="$ROOT/data"
P_IP=127.0.0.2; S_IP=127.0.0.3; PORT=9090
LOGDIR="$(mktemp -d)"
PIDS=(); PASS=0; FAIL=0
[ -x "$BIN/server" ] || make -C "$ROOT" >/dev/null || exit 1

cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT

start_secondary() { "$BIN/server" -r secondary -b $S_IP -p $PORT -d "$DATA/secondary.db" "$@" >"$LOGDIR/secondary.log" 2>&1 & SEC_PID=$!; PIDS+=($SEC_PID); }
start_primary()   { "$BIN/server" -r primary -b $P_IP -p $PORT -d "$DATA/primary.db" -f $S_IP:$PORT "$@" >"$LOGDIR/primary.log" 2>&1 & PRI_PID=$!; PIDS+=($PRI_PID); }
client() { "$BIN/client" -p $P_IP:$PORT -s $S_IP:$PORT "$@"; }

check() {  # check "name" "haystack" "needle"
    if grep -q -- "$3" <<<"$2"; then echo "  PASS: $1"; PASS=$((PASS+1));
    else echo "  FAIL: $1 (wanted '$3')"; echo "$2" | sed 's/^/        /'; FAIL=$((FAIL+1)); fi
}
summary() { echo "== $PASS passed, $FAIL failed"; [ "$FAIL" -eq 0 ]; }
