exec </dev/null
S=${SH:-bash}; S=$(command -v $S)
cd /tmp
R="$S -r -c"
echo "== 1"; $R 'cd /; echo "pwd=$PWD"' 2>&1; echo rc=$?
echo "== 2"; $R 'PATH=/x; echo "p=$PATH"' 2>&1; echo rc=$?
echo "== 3"; $R 'unset SHELL; echo "s=$SHELL"' 2>&1; echo rc=$?
echo "== 4"; $R '/bin/echo hi' 2>&1; echo rc=$?
echo "== 5"; $R 'echo hi > /tmp/rr.txt; echo after' 2>&1; echo rc=$?
echo "== 6"; $R 'echo hi >> /tmp/rr.txt' 2>&1; echo rc=$?
echo "== 7"; $R 'exec /bin/echo x' 2>&1; echo rc=$?
echo "== 8"; $R 'command -p echo y' 2>&1; echo rc=$?
echo "== 9"; $R 'set +r; echo $-' 2>&1; echo rc=$?
echo "== 10"; $R 'shopt -u restricted_shell; shopt restricted_shell' 2>&1; echo rc=$?
echo "== 11"; $R '. /tmp/rr.sh' 2>&1; echo rc=$?
echo "== 12"; $R 'hash -p /bin/echo e' 2>&1; echo rc=$?
echo "== 13"; $R 'echo "$-"; shopt restricted_shell'
echo "== 14"; $R 'export PATH=/y' 2>&1; echo rc=$?
echo "== 15"; $R 'declare -p PATH SHELL ENV BASH_ENV HISTFILE 2>&1 | cut -c1-30'
echo "== 16"; $R 'cat < /etc/hostname > /dev/null; echo ok' 2>&1
echo "== 17"; $R 'echo x 2>/dev/null' 2>&1; echo rc=$?
echo "== 18"; $R 'f() { echo inf; }; f; ls /tmp/rr.txt >/dev/null 2>&1; echo rc=$?' 2>&1
echo "== 19"; $R 'enable -n echo; echo z' 2>&1; echo rc=$?
echo "== 20"; $R 'enable echo' 2>&1; echo rc=$?
echo end
