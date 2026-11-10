# A local made over its own call's prefix binding and then exported -- by
# export, declare -x or local -x, export -n after or not -- is passed down
# at the return, value, array and readonly mark: `f() { local w; export w;
# }; w=3 f` leaves w=3, exported. A local the binding merely exported goes
# as any local does, and an export of a callee's own local reaches no outer
# binding. tsh put back what the binding hid (finding 575).
exec 2>&1
f() { local w; export w; }; w=3 f; echo "1[${w-u}]"; declare -p w; unset w
f() { local w=5; export w; export -n w; }; w=3 f; echo "2[${w-u}]"; unset w
f() { local -x w; }; w=g; w=3 f; declare -p w; unset w
f() { local -a w=(1 2); export w; }; w=3 f; declare -p w; unset w
f() { local -r w; export w; }; w=3 f; declare -p w
k() { local r=7; }; r=3 k; echo "3[${r-u}]"
g() { local y; export y; }; h() { g; echo "h[${y-u}]"; }; y=3 h; echo "4[${y-u}]"
