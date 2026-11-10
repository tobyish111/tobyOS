# A FUNCTION IS OUTSIDE ITS CALLER'S LOOPS: bash's execute_function starts
# it at loop level 0 (compat > 43), so a `break` there is "only meaningful
# in a loop" -- said on stderr, status 0 -- and the caller's loop goes on;
# `break 2` in the function's own loop leaves that loop only; a subshell
# starts at 0 too. tsh's broke out of the caller's loops, and said nothing
# outside a loop, which is bash's POSIX mode only (finding 126; logs/b22
# brk2).
exec 2>&1
break; echo "top st=$?"
continue; echo "topc st=$?"
f() { if break; then echo hi; fi; echo "f st=$?"; }; f
k() { break; echo "k-after st=$?"; }; for i in 1 2; do k; echo "k $i"; done; echo "k-loop st=$?"
c() { continue; echo "c-after"; }; for i in 1 2; do c; echo "c $i"; done
h() { for j in a b; do break 2; done; echo "h-after $j"; }; for i in 1 2; do h; echo "h $i"; done
w() { while :; do return 3; done; }; for i in 1; do w; echo "w st=$?"; done
for i in 1 2; do ( break ); echo "sub $i"; done
set -o posix; break; echo "posix st=$?"; set +o posix
for i in 1 2; do for j in a b; do break 2; done; echo "never"; done; echo "nested $i$j"
