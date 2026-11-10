exec 2>&1
cd /tmp; rm -rf l3 l3o; mkdir -p l3 l3o; echo x > l3o/f; ln -s ../l3o l3/dl
echo "== busybox ls dl/"; busybox ls l3/dl/ 2>&1
echo "== busybox ls -L"; busybox ls -lL l3 2>&1 | sed 's/  */ /g' | cut -d' ' -f1,9-
echo "== cat through link"; cat l3/dl/f 2>&1
echo "== cd"; (cd l3/dl && pwd && ls) 2>&1
echo "== native ls dl/"; /bin/ls l3/dl/ 2>&1
echo "== test -d"; [ -d l3/dl ] && echo dir
rm -rf l3 l3o
