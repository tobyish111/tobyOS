# $(< WORD) takes WORD as a redirection does: brace-expanded, split and
# globbed, then one word -- one match is read; two matches, a.x{1,2} and an
# unquoted $f holding 'a b' or nothing are "WORD: ambiguous redirect",
# status 1 (finding 405).
exec 2>&1
d=$(mktemp -d); cd "$d" || exit
echo one > a.x1; v=$(< a.x*); echo "[$v] $?"
echo two > a.x2; v=$(< a.x*); echo "[$v] $?"
v=$(< a.x{1,2}); echo "[$v] $?"
echo three > 'a b'; f='a b'; v=$(< $f); echo "[$v] $?"; v=$(< "$f"); echo "[$v] $?"
f=; v=$(< $f); echo "[$v] $?"
echo four > 'q*'; v=$(< "q*"); echo "[$v] $?"
HOME=$d; v=$(< ~/a.x1); echo "[$v] $?"
cd /; rm -rf "$d"
