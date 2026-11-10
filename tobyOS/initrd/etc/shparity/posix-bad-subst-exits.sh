# In POSIX mode a shell that is not interactive exits on a bad
# substitution: 127 under -c, 1 from a script, and a subshell alone ends
# -- tsh dropped the line and went on (finding 480).
exec 2>&1
N() { sed 's|^[^:]*: line|SH: line|'; }
"$BASH" -o posix -c 'echo "${!$}"; echo after' 2>&1 | N; echo "rc=${PIPESTATUS[0]}"
"$BASH" -c 'echo "${!$}"; echo after' 2>&1 | N; echo "rc=${PIPESTATUS[0]}"
"$BASH" -o posix -c 'eval "echo \${a b}"; echo after' 2>&1 | N; echo "rc=${PIPESTATUS[0]}"
"$BASH" -o posix -c '( echo "${!$}"; echo in-sub ); echo after $?' 2>&1 | N
f=${TMP:-/tmp}/pbse.$$.sh
printf '%s\n' 'set -o posix' 'g() { echo "${!$}"; echo in-g; }' 'g' 'echo after' > "$f"
"$BASH" "$f" 2>&1 | N; echo "rc=${PIPESTATUS[0]}"
rm -f "$f"
