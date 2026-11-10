# In a restricted shell a remembered command path may not hold a `/` and
# must be found on PATH: `hash -p sh a` with no sh there is "hash: sh: not
# found", and `BASH_CMDS[a]=sh` "sh: not found" -- tsh remembered both
# and ran ./sh (finding 515).
exec 2>&1
d=$(mktemp -d); cd "$d" || exit; cp /bin/sh .
"$BASH" -c 'PATH=/nonexistent; set -r
hash -p /bin/sh sh; echo "rc=$?"
hash -p sh a; echo "rc=$?"
BASH_CMDS[x]=/bin/sh; echo "rc=$?"
BASH_CMDS[c]=sh; echo "rc=$?"
hash
a -c "echo ran-a"' 2>&1 | sed 's/^[^:]*: line [0-9]*: //'
"$BASH" -c 'PATH=/bin:/usr/bin; set -r
hash -p ls b; echo "rc=$?"; hash' 2>&1 | sed 's/^[^:]*: line [0-9]*: //'
cd /; rm -rf "$d"
