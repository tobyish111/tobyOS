# In POSIX mode a shell that is not interactive exits on an error in
# `$(( ))`: 127 under -c, 1 from a script -- tsh dropped the line and went
# on (finding 481). `(( ))`, `let` and a subshell's error do not end it.
exec 2>&1
N() { sed 's|^[^:]*: line|SH: line|'; }
"$BASH" -o posix -c 'echo $(( x+ ))
exit 0' 2>&1 | N; echo "rc=${PIPESTATUS[0]}"
"$BASH" -o posix -c 'echo $(( 1 / 0 )); echo same-line' 2>&1 | N; echo "rc=${PIPESTATUS[0]}"
"$BASH" -c 'echo $(( x+ )); echo same-line
echo next-line' 2>&1 | N; echo "rc=${PIPESTATUS[0]}"
f=${TMP:-/tmp}/paee.$$.sh
printf '%s\n' 'set -o posix' 'echo $(( 1 + ))' 'echo after' > "$f"
"$BASH" "$f" 2>&1 | N; echo "rc=${PIPESTATUS[0]}"
printf '%s\n' 'set -o posix' '( echo $(( 1 + )); echo in-sub ); echo after $?' '(( 1 + )); echo after2 $?' 'let "1 +"; echo after3 $?' > "$f"
"$BASH" "$f" 2>&1 | N; echo "rc=${PIPESTATUS[0]}"
rm -f "$f"
