# A -c string that an expansion error ends -- ${x:?}, set -u, a bad @
# transform, a posix-mode readonly assignment -- exits 127, through eval, `.`
# and a function too, as does a simple command it runs in the background or
# a pipeline; a ( ), a compound command there and a $( ) exit 1, and so
# does a script (finding 407).
exec 2>&1
d=$(mktemp -d); cd "$d" || exit
"$BASH" -c 'echo ${ABXD:?"unset"}; echo after' sh; echo "rc=$?"
"$BASH" -c 'set -u; echo $nope' sh; echo "rc=$?"
"$BASH" -c 'x=1; echo ${x@Z}' sh; echo "rc=$?"
"$BASH" --posix -c 'readonly r=1; r=2; echo after' sh; echo "rc=$?"
"$BASH" -c 'readonly r=1; r=2; echo after' sh; echo "rc=$?"
"$BASH" -c 'eval "echo \${ABXD:?gone}"' sh; echo "rc=$?"
"$BASH" -c 'f() { echo ${ABXD:?gone}; }; f' sh; echo "rc=$?"
"$BASH" -c '( echo ${ABXD:?gone} ); echo "sub=$?"' sh; echo "rc=$?"
"$BASH" -c 'x=$(echo ${ABXD:?gone}); echo "c=$?"' sh; echo "rc=$?"
"$BASH" -c 'echo ${ABXD:?gone} | cat; echo "p=${PIPESTATUS[0]}"' sh
"$BASH" -c '{ echo ${ABXD:?gone}; } | cat; echo "p=${PIPESTATUS[0]}"' sh
"$BASH" -c 'echo ${ABXD:?gone} & wait $!; echo "bg=$?"' sh
"$BASH" -c '( echo ${ABXD:?gone} ) & wait $!; echo "bg=$?"' sh
"$BASH" -c 'trap "echo trap \$?" EXIT; echo ${ABXD:?gone}' sh; echo "rc=$?"
echo 'echo ${ABXD:?gone}' > s.sh; "$BASH" s.sh; echo "rc=$?"; "$BASH" -c ". ./s.sh; echo after" sh; echo "rc=$?"
cd /; rm -rf "$d"
