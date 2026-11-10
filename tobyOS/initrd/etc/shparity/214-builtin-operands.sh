# Builtins' operands as bash takes them: command -v/-V succeed when any name
# is found and -V says which is not; builtin and . take --; cd and exit
# refuse extra operands (exit: and drops the line, status 1); continue
# with extra operands ends the loop; a bad for name is an error that goes
# on; return/exit take any number and keep its low byte; export -n
# unexports; a special builtin's name may be a function; help PATTERN.
exec 2>&1
cd "${TMPDIR:-/tmp}" || exit 1
command -v echo ZZZ; echo st=$?
command -v ZZZ echo; echo st=$?
command -v ZZZ; echo st=$?
command -V nonexistent; echo st=$?
command -V echo ZZZ; echo st=$?
builtin; echo st=$?
builtin --; echo st=$?
builtin -- false; echo st=$?
builtin -x; echo st=$?
mkdir -p d1; cd d1 d2; echo st=$?; pwd | sed "s,.*/,,"; rmdir d1
x=1; export x; export -n x; declare -p x; export -n zz; echo st=$?; export -n y=2; declare -p y
eval() { echo "eval func $*"; }; eval 'echo hi'; unset -f eval
help ZZZ; echo st=$?
help ec >/dev/null; echo st=$?
for - in a; do echo no; done; echo st=$?
for x in a b; do echo $x; continue 1 2; done; echo after-loop
for i.j in a; do echo no; done; echo st=$?
f() { return 300; }; f; echo r=$?
f() { return -1; }; f; echo r=$?
g() { return abc; echo no; }; g; echo r=$?
h() { return -- 7; }; h; echo r=$?
(exit 256); echo e=$?
(exit " 3 "); echo e=$?
(exit 99999999999); echo e=$?
(exit -- 4); echo e=$?
(exit 0x10); echo e=$?
(exit abc; echo no); echo e=$?
exit 7 8
echo after-too-many st=$?
