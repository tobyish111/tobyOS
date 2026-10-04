S=${SH:-bash}; S=$(command -v $S)
echo "== 1"; $S -c 'exec false; echo after'; echo "rc=$?"
echo "== 2"; $S -c 'exec nosuch_cmd; echo after' 2>&1; echo "rc=$?"
echo "== 3"; $S -c 'shopt -s execfail; exec nosuch_cmd; echo after rc=$?' 2>&1; echo "rc=$?"
echo "== 4"; $S -c 'exec -c env' | grep -c .
echo "== 5"; env -i $S -c 'exec -l /bin/sh -c "echo \$0"'
echo "== 6"; env -i X=1 $S -c 'exec -a myname /bin/sh -c "echo \$0 \$X"'
echo "== 7"; env -i $S -c '(exec env) | grep SHLVL'
echo "== 8"; env -i $S -c 'exec env' | grep SHLVL
echo "== 9"; $S -c 'exec -x env' 2>&1; echo "rc=$?"
echo "== 10"; $S -c 'f() { exec echo inf; }; f; echo after'; echo "rc=$?"
echo "== 11"; $S -c 'exec 3>&1; echo still' ; echo "rc=$?"
echo "== 12"; printf 'exec false\necho after\n' > /tmp/x12.sh; $S /tmp/x12.sh; echo "rc=$?"
echo "== 13"; $S -c 'exec /tmp; echo after' 2>&1; echo "rc=$?"
echo end
