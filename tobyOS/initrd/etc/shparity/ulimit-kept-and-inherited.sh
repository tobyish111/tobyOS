# `ulimit` sets the kernel's limits for this shell, which its children
# inherit: a bare value sets the soft and the hard limit, -S or -H the one,
# "hard" and "soft" stand for the limit as it is; a soft limit over the
# hard one is "cannot modify limit: Invalid argument", raising the open
# files past the descriptor table "Operation not permitted". A subshell's
# limit is its own. tsh read a fixed table and kept no set: `ulimit -n 128;
# ulimit -n` was 1024 (finding 648).
exec 2>&1
ulimit -n 128; echo "set: $(ulimit -n) $(ulimit -Sn) $(ulimit -Hn)"
ulimit -Sn 100; echo "soft: $(ulimit -Sn) hard: $(ulimit -Hn)"
ulimit -Sn 120; echo "soft up: $(ulimit -Sn)"
ulimit -Sn 200; echo "soft over hard: $?"
ulimit -Hn 50; echo "hard under soft: $? $(ulimit -Hn)"
ulimit -n hard; echo "hard: $(ulimit -n)"
ulimit -n 3000; echo "past the table: $?"
(ulimit -n 64; echo "subshell: $(ulimit -n)"); echo "after it: $(ulimit -n)"
x=$(ulimit -n 32; ulimit -n); echo "comsub: $x, after it: $(ulimit -n)"
"$BASH" -c 'echo "child: $(ulimit -n)"'
ulimit -c 0; echo "core: $(ulimit -c) $(ulimit -Hc)"
ulimit -s 4096; echo "stack: $(ulimit -s)"
ulimit -t 100 -f 1000; echo "cpu and file: $(ulimit -t) $(ulimit -f)"
ulimit -Sc 10; echo "core soft over hard: $?"
ulimit -a | grep -E 'open files|core file'
