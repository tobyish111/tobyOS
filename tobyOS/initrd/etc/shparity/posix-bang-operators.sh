# In POSIX mode `#` and `?` after `${!` are operators on $! itself, not an
# indirection through $# or $?: with no job `${!?}` is "!: parameter not
# set" and `${!#}` is empty -- tsh expanded $0 and the last argument
# (finding 475). Outside POSIX mode both are indirections.
exec 2>&1
N() { sed 's|^[^:]*: line|SH: line|'; }
"$BASH" -o posix -c 'echo "[${!?}]"; echo after' 2>&1 | N; echo "rc=${PIPESTATUS[0]}"
"$BASH" -o posix -c 'echo "[${!#}]" "[${!-d}]" "[${!+s}]"' x a b; echo "rc=$?"
"$BASH" -o posix -c ': & wait; x=$!; [ "${!?}" = "$x" ] && echo same-pid'; echo "rc=$?"
"$BASH" -c 'set -- a b c; echo "[${!#}]"'; echo "rc=$?"
"$BASH" -o posix -c 'v=x; x=y; echo "[${!v}]"'; echo "rc=$?"
