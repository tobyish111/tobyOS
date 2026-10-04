S=${SH:-bash}; S=$(command -v $S)
echo "== 1"; env -i $S -c '(env)' | grep SHLVL
echo "== 2"; env -i $S -c '(cd /; env)' | grep SHLVL
echo "== 3"; env -i $S -c 'x=$(env); echo "$x" | grep SHLVL; :'
echo "== 4"; env -i $S -c 'x=$(cd /; env); echo "$x" | grep SHLVL; :'
echo "== 5"; env -i $S -c 'cat <(env) | grep SHLVL; :'
echo "== 6"; env -i $S -c 'env & wait' | grep SHLVL
echo "== 7"; env -i $S -c 'echo $SHLVL & wait'
echo "== 8"; env -i $S -c '(env >/dev/stderr) 2>&1 | grep SHLVL; :'
echo "== 9"; env -i $S -c 'env >/dev/null; env 2>/dev/null | grep SHLVL; :'
echo "== 10"; env -i $S -c 'env 2>/dev/null' | grep SHLVL
echo "== 11"; env -i $S -c 'trap "" INT; env' | grep SHLVL
echo "== 12"; env -i $S -c 'command env' | grep SHLVL
echo "== 13"; env -i $S -c 'command -p env' | grep SHLVL
echo "== 14"; env -i $S -c 'f() { :; }; env' | grep SHLVL
echo "== 15"; env -i $S -c 'eval env' | grep SHLVL
echo "== 16"; env -i $S -c 'exec env' | grep SHLVL
echo "== 17"; env -i $S -c 'echo $SHLVL; exec $0 -c "echo \$SHLVL"'
echo "== 18"; env -i $S -c 'x=1 env' | grep SHLVL
echo "== 19"; env -i $S -c 'env
' | grep SHLVL
echo "== 20"; env -i $S -c 'env

' | grep SHLVL
echo "== 21"; env -i $S -c 'env # c' | grep SHLVL
echo "== 22"; env -i $S -c 'false || env' | grep SHLVL
echo "== 23"; env -i $S -c 'true; true && env' | grep SHLVL
echo "== 24"; env -i $S -c 'echo $$ $(sh -c "echo \$PPID")' | awk '{print ($1==$2)?"same":"diff"}'
echo "== 25"; env -i $S -c 'sh -c "echo \$PPID" ' > /tmp/pp.txt; echo "ppid-is-shell=$( [ "$(cat /tmp/pp.txt)" = "$$" ] && echo yes || echo no)"
echo end
