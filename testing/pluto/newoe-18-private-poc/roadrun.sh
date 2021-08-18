../../guestbin/ping-once.sh --down -I 192.1.3.209 192.1.2.23
# wait on OE retransmits and rekeying
sleep 5
# should show tunnel and no shunts
ipsec whack --trafficstatus
ipsec whack --shuntstatus
../../guestbin/ipsec-look.sh
killall ip > /dev/null 2> /dev/null
cp /tmp/xfrm-monitor.out OUTPUT/road.xfrm-monitor.txt
# ping should succeed through tunnel
../../guestbin/ping-once.sh --up -I 192.1.3.209 192.1.2.23
echo done