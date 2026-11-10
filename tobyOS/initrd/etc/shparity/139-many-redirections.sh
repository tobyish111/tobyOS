f=${TMPDIR:-/tmp}/rd.$$
x=$(echo hi 3>/dev/null 4>/dev/null 5>/dev/null 6>/dev/null 7>/dev/null 8>/dev/null 9>/dev/null 3>/dev/null 4>/dev/null 5>/dev/null 6>/dev/null 7>/dev/null); echo "r1 [$x]"
echo body >"$f" 2>/dev/null 3>/dev/null 4>/dev/null 5>/dev/null 6>/dev/null 7>/dev/null 8>/dev/null 9>/dev/null 3>&1 4>&1; echo "r2 [$(cat "$f")]"
{ echo g1; echo g2 >&2; } 2>&1 3>/dev/null 4>/dev/null 5>/dev/null 6>/dev/null 7>/dev/null 8>/dev/null 9>/dev/null 3>/dev/null | sed 's/^/r3 /'
rm -f "$f"
echo done
