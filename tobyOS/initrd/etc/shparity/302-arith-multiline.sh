# AN ARITHMETIC COMMAND OVER SEVERAL LINES is one expression: its lines
# keep their newlines, indentation and `#`, and it runs on the line of its
# `))`. tsh joined them with "; " -- `(( c = 1 +` / `2 ))` was a syntax
# error -- and named the first line (finding 42; logs/b17 arithml).
exec 2>&1
# the shell under test: the gate sets no SH (probe3 does); $BASH is its own path
SH=${SH:-$BASH}
f=${TMP:-/tmp}/arithml.$$.sh
r() { $SH "$f" 2>&1 | sed "s#$f#SCRIPT#; s|^[/a-z]*: |SH: |"; echo "rc=${PIPESTATUS[0]}"; }
printf '(( a = 3 + 4  # comment\n   + ))\necho "st=$?"\n' > "$f"; r
printf '\n(( a = 1 +\n  2 +\n  1/0 ))\necho "st=$?"\n' > "$f"; r
printf 'if true; then\n  (( b = 2 *\n     ))\nfi\necho "st=$?"\n' > "$f"; r
printf 'echo $(( 1 +\n  * 2 ))\necho "st=$?"\n' > "$f"; r
printf '(( c = 1 +\n  2 ))\necho "c=$c"\n' > "$f"; r
printf '(( d = LINENO +\n  0 ))\necho "d=$d"\n' > "$f"; r
printf 'x=$(( LINENO +\n  0 ))\necho "x=$x"\n' > "$f"; r
printf '((\n  e = 5\n))\necho "e=$e"\n' > "$f"; r
printf '(( g = 1\n# not a comment here\n))\necho "st=$? g=$g"\n' > "$f"; r
printf '( (echo sub1\necho sub2) )\n' > "$f"; r
printf '((echo n1\necho n2) )\n' > "$f"; r
printf 'while (( i++ <\n  2 )); do echo "i=$i"; done\n' > "$f"; r
rm -f "$f"
echo end
