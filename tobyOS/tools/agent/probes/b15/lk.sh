exec 2>&1
cd /tmp; rm -rf lk; mkdir lk; cd lk; echo hi > f; ln -s f l; ln -s nosuch dang; mkdir d; ln -s d dl
echo "== native ls -l"; /bin/ls -l
echo "== busybox ls -l"; busybox ls -l 2>&1 | sed 's/  */ /g'
echo "== stat via bash test"; for t in "-L l" "-h l" "-f l" "-d dl" "-L dang" "-e dang" "-p /dev/stdin" "-c /dev/stdin" "-L /dev/stdin" "-e /dev/stdin" "-L /proc/self/fd/0"; do [ $t ] && echo "test $t: y" || echo "test $t: n"; done
echo "== readlink"; readlink l; readlink /dev/stdin; readlink /proc/self/fd/0; readlink /proc/self/fd/1
echo "== busybox stat"; busybox stat -c '%n %F %s' l dl d f dang /dev/stdin 2>&1
echo "== busybox stat -L"; busybox stat -L -c '%n %F' l dl /dev/stdin 2>&1
echo "== ls /proc/self/fd"; busybox ls -l /proc/self/fd 2>&1 | sed 's/  */ /g' | cut -d' ' -f1,9-
echo "== ls -l /dev/std*"; /bin/ls -l /dev/stdin /dev/stdout /dev/fd 2>&1
