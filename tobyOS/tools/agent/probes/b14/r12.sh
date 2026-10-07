exec 2>&1 </dev/null
# AN INTERACTIVE SHELL WITH NO TERMINAL (finding 61): bash's
# initialize_job_control finds none -- /dev/tty is ENXIO without a
# controlling terminal, fd 2 is a pipe -- says so twice, and has no `m`.
# The probe's own SSH command session has no terminal either (sshd gives
# `ssh host cmd` none, as on Linux).
N() { sed "s/^$1:/SH:/"; }
nm=${SH##*/}
echo "== setsid, -i, stdin /dev/null"
setsid -w $SH -i -c 'echo "[$-]"' < /dev/null 2>&1 | N $nm
echo "== setsid, -m"
setsid -w $SH -m -c 'echo "[$-]"' < /dev/null 2>&1 | N $nm
echo "== no setsid, -i, stderr a pipe"
$SH -i -c 'echo "[$-]"' < /dev/null 2>&1 | N $nm
echo "== interactive commands from a pipe, then set -m"
printf 'echo "[$-]"\nset -m; echo "rc=$? [$-]"\n' | $SH --norc -i 2>&1 | N $nm
echo "== /dev/tty here"
cat /dev/tty 2>&1 | sed 's/^[^:]*: //'
echo "== /dev/stdin, /dev/stdout, /dev/stderr"
echo via-stdin | cat /dev/stdin
echo via-redirect | $SH -c 'cat < /dev/stdin'
echo to-stdout > /dev/stdout
echo to-stderr > /dev/stderr
echo end
