"""OS-Ken OpenFlow 1.3 controller for the distributed DNS Mininet topologies.

* Baseline behaviour is retained: L2 learning switch with a table-miss rule.
* Redundant mode (env DNS_TOPOLOGY=redundant): s1 and s2 are joined by two
  parallel trunks.  The controller
    - tracks port/link state (OFPT_PORT_STATUS + port-description),
    - lets exactly ONE usable trunk carry traffic and never floods onto
      the standby trunk or from trunk to trunk  -> no loops/broadcast storms,
    - on a trunk failure/recovery picks a usable trunk, flushes stale flows
      and MAC-table entries on every switch, and logs the failover.
  Failover is controller-driven (it reacts in packet-in/port-status time,
  typically sub-second); it does NOT use OpenFlow fast-failover groups.
* A controller crash is not covered (that needs a separate backup controller).

Run:
  DNS_TOPOLOGY=redundant osken-manager --ofp-tcp-listen-port 6653 dns_controller.py
"""
import os

from os_ken.base import app_manager
from os_ken.controller import ofp_event
from os_ken.controller.handler import (CONFIG_DISPATCHER, MAIN_DISPATCHER,
                                       DEAD_DISPATCHER, set_ev_cls)
from os_ken.ofproto import ofproto_v1_3
from os_ken.lib.packet import packet, ethernet, ether_types

REDUNDANT = os.environ.get("DNS_TOPOLOGY", "single").lower() == "redundant"

# dpid -> trunk ports.  Index i on s1 is wired to index i on s2
# (must match topology/dns_topo.py).
TRUNKS = {1: [2, 3], 2: [3, 4]} if REDUNDANT else {}
LINK_NAMES = ["A", "B"]
FLOW_IDLE_TIMEOUT = 60


class DNSController(app_manager.OSKenApp):
    OFP_VERSIONS = [ofproto_v1_3.OFP_VERSION]

    def __init__(self, *args, **kwargs):
        super(DNSController, self).__init__(*args, **kwargs)
        self.mac_to_port = {}      # dpid -> {mac: port}
        self.datapaths = {}        # dpid -> datapath
        self.ports = {}            # dpid -> {port_no: is_up}
        self.active_link = 0 if REDUNDANT else None
        self.logger.info("DNS controller started, topology mode: %s",
                         "redundant" if REDUNDANT else "single")

    # ------------------------------------------------------------ helpers
    def add_flow(self, datapath, priority, match, actions, idle_timeout=0):
        ofp, parser = datapath.ofproto, datapath.ofproto_parser
        inst = [parser.OFPInstructionActions(ofp.OFPIT_APPLY_ACTIONS, actions)]
        datapath.send_msg(parser.OFPFlowMod(
            datapath=datapath, priority=priority, match=match,
            instructions=inst, idle_timeout=idle_timeout))

    def install_table_miss(self, datapath):
        ofp, parser = datapath.ofproto, datapath.ofproto_parser
        actions = [parser.OFPActionOutput(ofp.OFPP_CONTROLLER, ofp.OFPCML_NO_BUFFER)]
        self.add_flow(datapath, 0, parser.OFPMatch(), actions)

    @staticmethod
    def port_is_up(ofp, p):
        return not (p.state & ofp.OFPPS_LINK_DOWN) and not (p.config & ofp.OFPPC_PORT_DOWN)

    def active_port(self, dpid):
        trunks = TRUNKS.get(dpid, [])
        if self.active_link is None or not trunks:
            return None
        return trunks[self.active_link]

    def link_usable(self, i):
        return all(self.ports.get(dpid, {}).get(t[i], True) for dpid, t in TRUNKS.items())

    def update_active_link(self, reason):
        """Keep the current trunk while usable, else switch; flush on change."""
        if not REDUNDANT:
            return
        usable = [i for i in range(len(LINK_NAMES)) if self.link_usable(i)]
        old = self.active_link
        if old in usable:
            new = old
        else:
            new = usable[0] if usable else None
        if new == old:
            return
        self.active_link = new
        if new is None:
            self.logger.warning("FAILOVER: %s -> NO usable s1-s2 path!", reason)
        else:
            self.logger.warning("FAILOVER: %s -> traffic moved from trunk %s to trunk %s",
                                reason, LINK_NAMES[old] if old is not None else "none",
                                LINK_NAMES[new])
        self.flush_all()

    def flush_all(self):
        """Remove every learned flow and MAC entry, then re-add table-miss."""
        self.mac_to_port.clear()
        for dp in self.datapaths.values():
            ofp, parser = dp.ofproto, dp.ofproto_parser
            dp.send_msg(parser.OFPFlowMod(
                datapath=dp, command=ofp.OFPFC_DELETE, table_id=ofp.OFPTT_ALL,
                out_port=ofp.OFPP_ANY, out_group=ofp.OFPG_ANY,
                match=parser.OFPMatch()))
            self.install_table_miss(dp)
        self.logger.info("Flushed stale flows on %d switch(es)", len(self.datapaths))

    def flood_ports(self, dpid, in_port):
        """Loop-free flood set: all up ports except in_port, but only the
        active trunk, and never trunk -> trunk."""
        trunks = TRUNKS.get(dpid, [])
        active = self.active_port(dpid)
        out = []
        for port, up in sorted(self.ports.get(dpid, {}).items()):
            if port == in_port or not up:
                continue
            if port in trunks:
                if in_port in trunks or port != active:
                    continue
            out.append(port)
        return out

    # ------------------------------------------------------ switch events
    @set_ev_cls(ofp_event.EventOFPSwitchFeatures, CONFIG_DISPATCHER)
    def switch_features_handler(self, ev):
        dp = ev.msg.datapath
        parser = dp.ofproto_parser
        self.datapaths[dp.id] = dp
        self.install_table_miss(dp)
        dp.send_msg(parser.OFPPortDescStatsRequest(dp, 0))
        self.logger.info("Switch %s connected; table-miss flow installed", dp.id)

    @set_ev_cls(ofp_event.EventOFPStateChange, [MAIN_DISPATCHER, DEAD_DISPATCHER])
    def state_change_handler(self, ev):
        dp = ev.datapath
        if ev.state == DEAD_DISPATCHER and dp is not None and dp.id in self.datapaths:
            self.logger.warning("Switch %s disconnected", dp.id)
            del self.datapaths[dp.id]
            self.mac_to_port.pop(dp.id, None)

    @set_ev_cls(ofp_event.EventOFPPortDescStatsReply, MAIN_DISPATCHER)
    def port_desc_handler(self, ev):
        dp = ev.msg.datapath
        ofp = dp.ofproto
        table = self.ports.setdefault(dp.id, {})
        for p in ev.msg.body:
            if p.port_no < ofp.OFPP_MAX:
                table[p.port_no] = self.port_is_up(ofp, p)
        self.logger.info("Switch %s ports: %s", dp.id,
                         {k: ("up" if v else "down") for k, v in sorted(table.items())})
        self.update_active_link("port discovery on s%s" % dp.id)

    @set_ev_cls(ofp_event.EventOFPPortStatus, MAIN_DISPATCHER)
    def port_status_handler(self, ev):
        msg = ev.msg
        dp = msg.datapath
        ofp = dp.ofproto
        port = msg.desc.port_no
        if port >= ofp.OFPP_MAX:
            return
        table = self.ports.setdefault(dp.id, {})
        if msg.reason == ofp.OFPPR_DELETE:
            up = False
            table.pop(port, None)
        else:
            up = self.port_is_up(ofp, msg.desc)
            table[port] = up
        trunks = TRUNKS.get(dp.id, [])
        if port in trunks:
            name = LINK_NAMES[trunks.index(port)]
            self.logger.warning("LINK EVENT: s%s port %s (trunk %s) is now %s",
                                dp.id, port, name, "UP" if up else "DOWN")
            table[port] = up        # explicit even after OFPPR_DELETE
            self.update_active_link("trunk %s %s" % (name, "recovered" if up else "failed"))
        elif not up:
            # access port went away: forget hosts learned there, drop flows to it
            self.logger.warning("LINK EVENT: s%s access port %s is DOWN", dp.id, port)
            macs = self.mac_to_port.get(dp.id, {})
            for m in [m for m, p in macs.items() if p == port]:
                del macs[m]
            parser = dp.ofproto_parser
            dp.send_msg(parser.OFPFlowMod(
                datapath=dp, command=ofp.OFPFC_DELETE, table_id=ofp.OFPTT_ALL,
                out_port=port, out_group=ofp.OFPG_ANY, match=parser.OFPMatch()))

    # ------------------------------------------------------------ packets
    @set_ev_cls(ofp_event.EventOFPPacketIn, MAIN_DISPATCHER)
    def packet_in_handler(self, ev):
        msg = ev.msg
        dp = msg.datapath
        ofp, parser = dp.ofproto, dp.ofproto_parser
        dpid = dp.id
        in_port = msg.match["in_port"]

        eth = packet.Packet(msg.data).get_protocol(ethernet.ethernet)
        if eth is None or eth.ethertype == ether_types.ETH_TYPE_LLDP:
            return

        trunks = TRUNKS.get(dpid, [])
        active = self.active_port(dpid)
        if in_port in trunks and in_port != active:
            return                      # standby trunk: ignore (loop prevention)

        src, dst = eth.src, eth.dst
        self.mac_to_port.setdefault(dpid, {})[src] = in_port

        out_port = self.mac_to_port[dpid].get(dst)
        if out_port is not None and out_port in trunks and out_port != active:
            out_port = None             # stale entry pointing at the standby trunk
        if out_port is not None and not self.ports.get(dpid, {}).get(out_port, True):
            out_port = None

        if out_port is not None and out_port != in_port:
            actions = [parser.OFPActionOutput(out_port)]
            self.add_flow(dp, 10, parser.OFPMatch(in_port=in_port, eth_dst=dst),
                          actions, idle_timeout=FLOW_IDLE_TIMEOUT)
        elif out_port == in_port and out_port is not None:
            return
        else:
            known = self.flood_ports(dpid, in_port)
            if known:
                actions = [parser.OFPActionOutput(p) for p in known]
            elif not self.ports.get(dpid):
                actions = [parser.OFPActionOutput(ofp.OFPP_FLOOD)]   # ports unknown
            else:
                return

        data = msg.data if msg.buffer_id == ofp.OFP_NO_BUFFER else None
        dp.send_msg(parser.OFPPacketOut(datapath=dp, buffer_id=msg.buffer_id,
                                        in_port=in_port, actions=actions, data=data))
