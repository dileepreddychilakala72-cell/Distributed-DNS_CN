#!/bin/bash
# Same request ID sent twice -> server answers both consistently, second from cache.
. "$(dirname "$0")/common.sh"
echo "== test_duplicate"
start_secondary; start_primary; sleep 0.3
out=$(python3 - "$P_IP" "$PORT" <<'PY'
import socket, struct, sys
ip, port = sys.argv[1], int(sys.argv[2])
def msg(t, rid, dom, flags=0):
    return struct.pack("!BBBBI64s16s", 1, t, flags, 0, rid, dom.encode(), b"")
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(1)
res = []
for _ in range(2):                       # identical request, identical ID
    s.sendto(msg(1, 4242, "wikipedia.org"), (ip, port))
    d, _ = s.recvfrom(512)
    v, t, f, st, rid, dom, a = struct.unpack("!BBBBI64s16s", d)
    res.append((rid, st, a.rstrip(b"\0").decode()))
print("SAME" if res[0] == res[1] else "DIFFERENT", res[0])
# wrong-ID sanity: a different ID gets its own (fresh) answer
s.sendto(msg(1, 7, "google.com"), (ip, port)); d, _ = s.recvfrom(512)
print("ID-ECHO", struct.unpack("!BBBBI64s16s", d)[4])
PY
)
check "duplicate request -> identical response" "$out" "SAME (4242, 0, '208.80.154.224')"
check "request ID echoed"                       "$out" "ID-ECHO 7"
check "second copy served from cache"           "$(cat $LOGDIR/primary.log)" "CACHE-HIT id=4242"
check "forwarded to secondary only once"        "$(grep -c 'FORWARD to secondary' $LOGDIR/primary.log)" "^1$"
summary
