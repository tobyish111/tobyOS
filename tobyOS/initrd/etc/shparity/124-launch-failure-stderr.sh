f=${TMPDIR:-/tmp}/lf.$$
nosuch_d1 2>"$f"; echo "D1 st=$?"
if [ -s "$f" ]; then echo "D1 message in the file"; else echo "D1 file empty"; fi
x=$(nosuch_d2 2>&1); if [ -n "$x" ]; then echo "D2 captured"; else echo "D2 empty"; fi
nosuch_d3 2>/dev/null; echo "D3 st=$?"
x=$(nosuch_d4 2>&1 >/dev/null); if [ -n "$x" ]; then echo "D4 captured"; else echo "D4 empty"; fi
x=$(nosuch_d6 2>/dev/null); echo "D6 [$x]"
rm -f "$f"
echo done
