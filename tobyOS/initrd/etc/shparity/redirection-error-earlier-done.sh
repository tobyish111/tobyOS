# bash does a command's redirections in order, so when one's word fails to
# expand, those before it are done: their files are made and emptied, and
# the error is said where they have sent stderr -- an external command's in
# the child it forked, a builtin's and a function's in the shell. tsh said
# it on its own stderr and made nothing (finding 643).
exec 2>&1
echo old > o1; cat > o1 <<< ${a[}; echo "cat >o1: $?, o1 has $(wc -c < o1) bytes"
rm -f o2; cat 3> o2 <<< ${a[}; echo "cat 3>o2: $?"; ls o2
cat 2> e1 <<< ${a[}; echo "cat 2>e1: $?, e1 says: $(sed 's/^.*line/line/' e1)"
f() { cat; }; f 2> e2 <<< ${a[}; echo "not after f"
echo "f 2>e2: $?, e2 says: $(sed 's/^.*line/line/' e2)"
rm -f o3; echo hi > o3 < ./in${a[}; echo "not after echo"
echo "echo >o3: $?"; ls o3
rm -f o4; cat <<< ${a[} > o4; echo "cat, o4 after it: $?"; ls o4
