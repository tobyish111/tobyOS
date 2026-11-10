exec </dev/null
cd /tmp
r() { sed "s#$SH#SH#g; s#/bin/SH#SH#g"; }
echo "== stdout only"
printf 'echo hi\nexit 3\n' | setsid $SH -i 2>/dev/null; echo "rc=$?"
echo "== stderr only"
printf 'echo hi\nexit 3\n' | setsid $SH -i 2>&1 >/dev/null | r; echo "rc=$?"
echo "== eof"
printf 'echo hi\n' | setsid $SH -i 2>&1 >/dev/null | r; echo "rc=$?"
echo "== PS2"
printf 'echo "a\nb"\n' | setsid $SH -i 2>&1 >/dev/null | r; echo "rc=$?"
echo "== exit trap order"
printf 'trap "echo trapped" EXIT\nexit 4\n' | setsid $SH -i 2>&1 | r; echo "rc=$?"
echo "== login"
printf 'echo hi\nexit\n' | setsid $SH -l -i 2>&1 >/dev/null | r; echo "rc=$?"
echo "== -i -c"
setsid $SH -i -c 'echo ic' 2>&1 | r; echo "rc=$?"
echo end
