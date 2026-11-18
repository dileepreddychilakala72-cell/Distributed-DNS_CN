import sys, types, os
from unittest import mock
os.environ["DNS_TOPOLOGY"]="redundant"
for m in ["os_ken","os_ken.base","os_ken.controller","os_ken.controller.ofp_event","os_ken.controller.handler","os_ken.ofproto","os_ken.ofproto.ofproto_v1_3","os_ken.lib","os_ken.lib.packet"]:
    sys.modules[m]=mock.MagicMock()
class Base:
    def __init__(self,*a,**k):
        import logging; self.logger=logging.getLogger("ctl"); logging.basicConfig(level=logging.INFO,format="%(message)s")
sys.modules["os_ken.base"].app_manager.OSKenApp=Base
sys.modules["os_ken.controller.handler"].set_ev_cls=lambda *a,**k:(lambda f:f)
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "controller"))
import dns_controller as d
c=d.DNSController()
def dp(i):
    x=mock.MagicMock(); x.id=i; x.ofproto.OFPP_MAX=0xffffff00; return x
c.datapaths={1:dp(1),2:dp(2)}
c.ports={1:{1:True,2:True,3:True},2:{1:True,2:True,3:True,4:True}}
assert c.flood_ports(1,1)==[2], c.flood_ports(1,1)         # h1 ARP -> only active trunk A
assert c.flood_ports(2,3)==[1,2], c.flood_ports(2,3)       # from trunk A -> hosts only, no trunk B
assert c.flood_ports(2,1)==[2,3], c.flood_ports(2,1)       # from h2 -> h3 + trunk A (not B)
print("flood sets OK")
c.ports[1][2]=False; c.update_active_link("test")           # trunk A fails
assert c.active_link==1 and c.flood_ports(1,1)==[3] and c.flood_ports(2,1)==[2,4]
c.ports[2][4]=False; c.update_active_link("test")           # B fails too
assert c.active_link is None and c.flood_ports(1,1)==[]
c.ports[1][2]=True; c.update_active_link("test")            # A back
assert c.active_link==0
c.ports[1][3]=True; c.ports[2][4]=True; c.update_active_link("test")
assert c.active_link==0                                     # sticky, no flap
print("failover/recovery OK; flow-delete msgs sent:", sum(x.send_msg.call_count for x in c.datapaths.values()))
