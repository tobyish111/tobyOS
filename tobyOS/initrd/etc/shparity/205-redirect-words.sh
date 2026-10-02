# A redirection's word is ONE filename: two from brace expansion, word
# splitting or a glob is "WORD: ambiguous redirect", status 1. `&>word` and
# `&>>word` take stdout and stderr; `|&` is `2>&1 |`. tsh wrote the file
# `a-{one,two}` -- or `a-one` with `a-two` an argument -- and read `&>` and
# `|&` as a background operator.
exec 2>&1
cd "${TMPDIR:-/tmp}" || exit 1
echo hi > a-{one,two}; echo "1 $?"
v='p q'; echo hi > $v; echo "2 $?"
echo hi > b-{1..1}; cat b-1; rm -f b-1
2&>1; echo "3 $?"; cat 1 | wc -l; rm -f 1
echo out &> o.txt; echo "4 $?"; cat o.txt
ls /nonexist-zz &> e.txt; echo "5 $?"; wc -l < e.txt
echo more &>> o.txt; cat o.txt
{ echo a; echo b >&2; } &> g.txt; sort g.txt
set -o noclobber; echo x &> o.txt; echo "6 $?"; set +o noclobber
se() { echo STDOUT; echo STDERR >&2; }
se |& sort
{ echo g1; echo g2 >&2; } |& sort
se |& cat > p.txt; sort p.txt
echo x | cat |& cat
rm -f o.txt e.txt g.txt p.txt
