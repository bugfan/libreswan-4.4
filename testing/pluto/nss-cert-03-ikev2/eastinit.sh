/testing/guestbin/swan-prep --x509
certutil -d sql:/etc/ipsec.d -D -n west
ipsec start
../../guestbin/wait-until-pluto-started
ipsec auto --add nss-cert
ipsec auto --status |grep nss-cert
echo "initdone"
