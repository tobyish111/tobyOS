# paren-values.sh -- a VALUE that begins with `(` is a string, wherever it
# is assigned from: only an assignment WORD `a=(...)` is an array literal.
echo "(( x"; echo "1 $_"
for v in "(a b)"; do declare -p v; done
read r <<< "(a b)"; declare -p r
printf -v p '(%s)' 'a b'; declare -p p
x='(a b)'; y=$x; declare -p y
getopts 'a:' o -a '(q)'; declare -p OPTARG
f() { echo "(last word)"; }; f; echo "2 $_"
mapfile -t m <<< "(m n)"; declare -p m
: "(u v)"; echo "3 $_"
