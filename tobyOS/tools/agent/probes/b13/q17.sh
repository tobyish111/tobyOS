exec 2>&1
cp /bin/true /tmp/t1; chmod 755 /tmp/t1
cd /tmp && ./t1; echo "tmp rc=$?"
cd /bin && /bin/true; echo "abs rc=$?"
cd /bin && ../bin/true; echo "dotdot rc=$?"
cd / && bin/true; echo "rel rc=$?"
cd / && ./bin/true; echo "dotrel rc=$?"
mkdir -p /tmp/dd; cp /bin/true /tmp/dd/x; cd /tmp && dd/x; echo "sub rc=$?"
cd /tmp
printf '#!/bin/sh\necho script-ok\n' > /tmp/s1.sh; chmod 755 /tmp/s1.sh; ./s1.sh
ls -la /bin/true /bin/env | head -3
x=g; export x; f() { local x=l; env | grep ^x=; }; f
echo end
