# confirm received delete was processed - should show no tunnel
ipsec whack --trafficstatus
../../guestbin/wait-for.sh --no-match ' spi 0x00000000 ' -- ip xfrm state
# try triggering again, ondemand policy should re-trigger OE
ip xfrm state
ip xfrm pol
../../guestbin/ping-once.sh --forget -I 192.1.3.209 192.1.2.23
../../guestbin/wait-for.sh --match private-or-clear -- ipsec whack --trafficstatus
# should show established tunnel and no bare shunts
../../guestbin/wait-for.sh --no-match ' spi 0x00000000 ' -- ip xfrm state
ip xfrm state
ip xfrm pol
ipsec whack --trafficstatus
ipsec whack --shuntstatus
# ping should succeed through tunnel
../../guestbin/ping-once.sh --up -I 192.1.3.209 192.1.2.23
ipsec whack --trafficstatus
