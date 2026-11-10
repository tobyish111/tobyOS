# Does a cd or a prefix assignment inside $( ) reach the parent shell?
cd /tmp; rm -rf cdl.$$; mkdir -p cdl.$$/box/sub cdl.$$/inner; cd cdl.$$; base=$PWD
out=$(cd box); echo "after \$(cd box): [${PWD#$base}]"
out=$(CDPATH=$base/box cd sub); echo "after \$(CDPATH=.. cd sub): [${PWD#$base}] out=[${out#$base}]"
x=$(CDPATH=/x true); echo "CDPATH after: [${CDPATH-unset}]"
cd inner && echo "cd inner works"
cd $base; ( cd box ); echo "after ( cd box ): [${PWD#$base}]"
cd /tmp; rm -rf cdl.$$
echo done
