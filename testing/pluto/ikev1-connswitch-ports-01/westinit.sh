/testing/guestbin/swan-prep
ipsec start
../../guestbin/wait-until-pluto-started
ipsec auto --add westnet-eastnet-port666
ipsec auto --add westnet-eastnet-port667
ipsec whack --impair suppress-retransmits
echo "initdone"
