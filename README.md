# Distributed DNS - UDP + Mininet + OpenFlow 1.3 + OS-Ken

Upgrade of the team's baseline (one-shot UDP client/server, one-switch Mininet,
learning-switch controller) following *Distributed DNS - Upgrade & Implementation Plan*.

```
distributed-dns/
  common/dns_protocol.h      shared 88-byte wire format (version|type|flags|status|id|domain|ip)
  client/client.c            timeouts, retries 1/2/4 s, request IDs, failover, outcomes
  server/server.c            primary/secondary roles, forwarding, response cache
  dispatcher/dispatcher.c    OPTIONAL round-robin load balancer + health checks
  controller/dns_controller.py   learning switch + link monitoring + trunk failover
  topology/dns_topo.py       baseline (default) and --redundant two-switch topology
  data/primary.db, secondary.db  records (secondary = replica of primary + extras)
  tests/                     test_normal / failover / server_failure / packet_loss /
                             duplicate / dispatcher / controller_logic
  Makefile
```

## Build and test (no Mininet needed)
```bash
make            # -> bin/client bin/server bin/dispatcher
make test       # loopback tests (127.0.0.2 = h2, 127.0.0.3 = h3)
python3 tests/test_controller_logic.py   # controller flood/failover logic (os_ken stubbed)
```

## Protocol (agreed once, in common/dns_protocol.h)
Fixed 88-byte datagram, big-endian, serialized field by field (no raw struct).
Types: QUERY, RESPONSE, PING, PONG. Status: OK, NOTFOUND, ERROR. Flag FORWARDED stops loops.
Ports: servers 9090, dispatcher 9000.

## Application behaviour
| Plan item | Implementation |
|---|---|
| Distributed resolution | primary: local DB, else forwards to secondary (2 x 500 ms) and relays |
| Independent secondary | answers direct + forwarded queries, never forwards; holds a replica of primary records |
| NOTFOUND only after lookup completes | if the forward fails the primary returns ERROR (not NOTFOUND); client then asks the secondary directly |
| Request IDs | random 32-bit per lookup, echoed by servers; client accepts only the matching ID, discards old/duplicate responses |
| Retries | per target `-r` attempts (default 3) with timeouts 1/2/4 s (`-t` base), overall deadline `-D` (default 15 s) |
| Failover | targets tried in order: [dispatcher], primary, secondary |
| Outcomes | SUCCESS(0) NOTFOUND(1) TIMEOUT(2) SERVER_UNAVAILABLE(3) (exit codes) |
| Duplicate-safe | servers cache (client, id) -> response for 30 s; errors are never cached |

## Run in Mininet
Terminal 1 (controller):
```bash
source ~/osken-venv/bin/activate
osken-manager --ofp-tcp-listen-port 6653 ~/distributed-dns/controller/dns_controller.py
# redundant topology: DNS_TOPOLOGY=redundant osken-manager ...
```
Terminal 2:
```bash
cd ~/distributed-dns && make
sudo mn -c
sudo python3 topology/dns_topo.py            # baseline, one switch
# or: sudo python3 topology/dns_topo.py --redundant
```
At `mininet>`:
```
pingall
h3 ./bin/server -r secondary -d data/secondary.db > /tmp/s.log 2>&1 &
h2 ./bin/server -r primary -d data/primary.db -f 10.0.0.3 > /tmp/p.log 2>&1 &
h1 ./bin/client google.com            # primary
h1 ./bin/client wikipedia.org         # forwarded to secondary
h1 ./bin/client nosuch.invalid        # NOTFOUND
h2 pkill server; h1 ./bin/client wikipedia.org   # timeout on h2 -> served by h3
```
Packet loss (confirm interface first):
```
h1 tc qdisc add dev h1-eth0 root netem loss 20%
h1 ./bin/client -t 200 -r 8 google.com
h1 tc qdisc del dev h1-eth0 root
```
Load balancing (dispatcher on h1, clients use it first):
```
h1 ./bin/dispatcher -b 127.0.0.1 10.0.0.2 10.0.0.3 > /tmp/d.log 2>&1 &
h1 ./bin/client -d 127.0.0.1 google.com     # repeat; see /tmp/d.log alternate h2/h3
```
Link failover (redundant topology, controller started with DNS_TOPOLOGY=redundant):
```
h1 ping 10.0.0.2 &                 # traffic flows over trunk A
sh ip link set s1-eth2 down        # controller logs "FAILOVER ... trunk A -> trunk B"
sh ovs-ofctl -O OpenFlow13 dump-flows s1
sh ip link set s1-eth2 up
```

## Design notes / honest limitations
- **Redundancy scope:** only the s1-s2 trunk is redundant (two parallel links). Host access
  links (h1-s1, h2/h3-s2) and the controller remain single points of failure; a backup
  controller is a separate feature and is not implemented.
- **Loop prevention:** the controller floods only onto the single active trunk and never
  trunk-to-trunk, so no STP is needed. Failover is controller-driven (port-status ->
  flush flows -> relearn); OpenFlow fast-failover groups are not used.
- **Servers are single-threaded:** a forwarding wait (<= 1 s) delays other queries; retransmitted
  queries are then answered from the cache.
- **Verification status:** C components and all `tests/test_*.sh` were compiled with `-Wall -Wextra`
  and passed on loopback, and the controller logic was unit-tested with os_ken stubbed. The Mininet
  topology, real OS-Ken run and OVS link-down behaviour could not be run here - please run the
  Mininet steps above and record results. Packet-loss success is probabilistic, not guaranteed.
