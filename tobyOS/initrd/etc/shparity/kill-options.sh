# kill's options and words as bash's kill_builtin reads them, and the
# signal table (display_signal_list) -- the real-time signals too, which
# trap -l listed none of.
k() { echo "== kill $*"; kill "$@" 2>&1; echo "rc=$?"; }
k
k -l
k -L
k -l 9 15 0 32 40 64 65 137 -1 abc rtmin+3 SIGRTMAX-2 RTMAX-14 debug ERR return exit sigint Int
k -l -- 9
k -l HUP TERM
k -L 2
k -9x 1
k -FOO 1
k -s FOO 1
k -n 99 1
k -s
k -n
k -?
k --
k -- abc
k ''
k -
k abc
k -s TERM
k %9
k 0x10
trap -l
echo "rc=$?"
# a live job: its pid printed as PID; signal 0 until the end, so nothing
# races the job's death
sleep 5 &
p=$!
kp() { echo "== kill ${*//$p/PID}"; out=$(kill "$@" 2>&1); rc=$?; printf '%s\n' "${out//$p/PID}"; echo "rc=$rc"; }
kp -0 $p
kp -s 0 $p
kp -s EXIT $p
kp -n 0 $p
kp -0 " $p "
kp -0 "+$p"
kp -0 999999 $p
kp -0 $p 999999
kp -s HUP 999999
kp -STOP $p
kp -sCONT $p
kp -n15 $p
wait $p
echo "wait=$?"
