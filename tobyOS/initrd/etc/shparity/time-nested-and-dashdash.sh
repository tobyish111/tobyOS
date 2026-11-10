# `time` takes -p and then `--` (which asks for the POSIX form too), and a
# `time` after them is the same timing, reported once: `time time -p echo`
# is one POSIX report -- tsh timed each level and ran `--` as a command
# (finding 492). The times themselves are left out.
exec 2>&1
TIMEFORMAT='T'
N() { sed 's/ [0-9][0-9.]*$/ N/'; }
{ time time echo a; } 2>&1 | N
{ time time -p echo b; } 2>&1 | N
{ time -p time echo c; } 2>&1 | N
{ time -p -- echo d; } 2>&1 | N
{ time -- echo e; } 2>&1 | N
echo "rc=$?"
