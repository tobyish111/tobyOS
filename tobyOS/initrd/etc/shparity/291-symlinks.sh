# Symbolic links are FILES: `test -L`/`-h` see them, `-e` and `-f` look
# through them, a dangling one exists only to -L, /dev/stdin and
# /proc/PID/fd/N are links naming the open file. tmpfs (/tmp) could not hold
# a link, the RAM link table was invisible to stat and nothing followed one
# (logs/b15 lk).
exec 2>&1
T=/tmp/sp291; rm -rf $T; mkdir $T; cd $T
echo hi > f; ln -s f l; ln -s nosuch dang; mkdir d; echo x > d/in; ln -s d dl
for t in "-L l" "-h l" "-f l" "-L f" "-d dl" "-L dl" "-L dang" "-h dang" \
         "-e dang" "-f dang" "-L /dev/stdin" "-L /dev/fd" "-d /dev/fd/" \
         "-L /proc/self/fd/0"; do
  [ $t ] && echo "[ $t ] y" || echo "[ $t ] n"
  test $t && echo "test $t y" || echo "test $t n"
done
[[ -L l ]] && echo "[[ -L l ]] y"; [[ -h d ]] || echo "[[ -h d ]] n"
[[ -e dang ]] || echo "[[ -e dang ]] n"
[ l -ef f ] && echo "l -ef f"; [ dl -ef d ] && echo "dl -ef d"
test -c /dev/stdin < /dev/null && echo "stdin char"
echo | { test -p /dev/stdin && echo "stdin pipe"; }
test -f /dev/stdin < f && echo "stdin file"
[ -e /dev/stdin ] && echo "stdin exists"; [ -e /dev/stderr ] && echo "stderr exists"
[ -e /dev/fd/7 ] || echo "fd 7 is not open"
[ -t 1 ] || echo "stdout not a tty"; [ -t 0 ] < f || echo "file not a tty"
echo | { [ -t 0 ] || echo "pipe not a tty"; }; [ -t 7 ] || echo "fd 7 not a tty"
echo "== readlink"; readlink l dang dl /dev/stdin /dev/stdout /dev/fd
readlink /proc/self/fd/0 < f
exec 3< f; readlink /proc/$$/fd/3; exec 3<&-
echo | readlink /proc/self/fd/0 | sed 's/[0-9][0-9]*/N/'
readlink f; echo "regular $?"
echo "== through"; cat l; cat < l; cat /dev/stdin < l; cat dl/in
echo more >> l; cat f
echo "== ls"; ls -F; ls dl; ls -l | sed 's/  */ /g' | cut -d' ' -f1,2,5,9-
rm l dl; ls; cat f d/in
cd /; rm -rf $T
echo end
