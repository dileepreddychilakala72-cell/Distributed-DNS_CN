#!/usr/bin/env python3
"""Mininet topology for the distributed DNS project.

  default (baseline, unchanged):   h1, h2, h3 -- s1
  --redundant:                     h1 -- s1 ==(2 links)== s2 -- h2, h3

Redundant mode port map (explicit, so the controller can rely on it):
  s1: port 1 = h1,  port 2 = trunk A, port 3 = trunk B
  s2: port 1 = h2,  port 2 = h3,      port 3 = trunk A, port 4 = trunk B
Trunk A = s1-eth2 <-> s2-eth3,  trunk B = s1-eth3 <-> s2-eth4.
All hosts share 10.0.0.0/24, so no routing is needed; loop prevention is
done by the controller (only one trunk carries traffic at a time).

Start the controller with  DNS_TOPOLOGY=redundant  for this mode.
"""
import argparse
from functools import partial

from mininet.topo import Topo
from mininet.net import Mininet
from mininet.node import RemoteController, OVSSwitch
from mininet.cli import CLI
from mininet.log import setLogLevel


class DNSTopo(Topo):
    """Baseline: three hosts on one OpenFlow switch."""

    def build(self):
        s1 = self.addSwitch("s1")
        h1 = self.addHost("h1", ip="10.0.0.1/24")
        h2 = self.addHost("h2", ip="10.0.0.2/24")
        h3 = self.addHost("h3", ip="10.0.0.3/24")
        self.addLink(h1, s1)
        self.addLink(h2, s1)
        self.addLink(h3, s1)


class RedundantDNSTopo(Topo):
    """Two switches joined by two parallel links (trunk A / trunk B)."""

    def build(self):
        s1 = self.addSwitch("s1")
        s2 = self.addSwitch("s2")
        h1 = self.addHost("h1", ip="10.0.0.1/24")
        h2 = self.addHost("h2", ip="10.0.0.2/24")
        h3 = self.addHost("h3", ip="10.0.0.3/24")
        self.addLink(h1, s1, port2=1)
        self.addLink(h2, s2, port2=1)
        self.addLink(h3, s2, port2=2)
        self.addLink(s1, s2, port1=2, port2=3)   # trunk A
        self.addLink(s1, s2, port1=3, port2=4)   # trunk B


def run(redundant):
    topo = RedundantDNSTopo() if redundant else DNSTopo()
    net = Mininet(topo=topo,
                  controller=None,
                  switch=partial(OVSSwitch, protocols="OpenFlow13"),
                  autoSetMacs=True)
    net.addController("c0", controller=RemoteController, ip="127.0.0.1", port=6653)
    net.start()
    print("DNS Mininet topology started (%s)" %
          ("REDUNDANT: 2 switches, 2 trunks" if redundant else "single switch"))
    print("h1 = DNS client     10.0.0.1")
    print("h2 = Primary server 10.0.0.2")
    print("h3 = Secondary      10.0.0.3")
    if redundant:
        print("Controller must run with DNS_TOPOLOGY=redundant")
        print("Fail trunk A :  sh ip link set s1-eth2 down")
        print("Restore it   :  sh ip link set s1-eth2 up")
    CLI(net)
    net.stop()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--redundant", action="store_true",
                    help="two switches with two parallel trunks")
    args = ap.parse_args()
    setLogLevel("info")
    run(args.redundant)
