# A function name is a WORD, not an identifier.
#
# tsh held function names to the VARIABLE rule -- letters, digits and
# underscore -- in four separate places, so a definition as ordinary as
#
#     osh-normalize() { sed 's/x/y/'; }
#
# was not recognised as a definition at all and the shell went looking for
# a program by that name. Oils has a whole file on this (spec/func-parsing)
# and every case in it is BASH-ONLY, so the oilspec gate shows no diff.
#
# The shapes below are the ones bash accepts, plus the two it refuses --
# the ones it would have to EXPAND to know the name of.

echo "== hyphen"
foo-bar() { echo ran-hyphen; }
foo-bar
type -t foo-bar

echo "== dot"
func-name.ext ( ) { echo ran-dot; }
func-name.ext

echo "== equals"
func-name=ext ( ) { echo ran-equals; }
func-name=ext

echo "== bang"
foo!bar() { echo ran-bang; }
foo!bar

echo "== slash"
ns/fn() { echo ran-slash; }
ns/fn

echo "== call through a pipe"
norm107() { sed 's/shell function/function/'; }
echo "a shell function b" | norm107

echo "== unset"
unset -f foo-bar
type -t foo-bar; echo "after-unset=$?"

echo "== not-a-function"
# `a=()` is an EMPTY ARRAY, not a function called `a=`. This is the one
# place the permissive word rule has to give way, and missing it cost seven
# cases in a single run -- empty arrays, sparse arrays, an empty COMP_ARGV
# and a named parse-error regression -- so the shapes are pinned here.
arr107=(); echo "empty n=${#arr107[@]}"
arr107=(a b); echo "two n=${#arr107[@]} 1=${arr107[1]}"
sparse107=([3]=x); echo "sparse n=${#sparse107[@]} 3=${sparse107[3]}"
arr107+=(c); echo "appended n=${#arr107[@]}"

echo "== in a subshell"
( f107sub() { echo in-sub; }; f107sub )

# The two bash refuses. Both are "not a valid identifier" -- an error at the
# DEFINITION, so status 1: not a syntax error, and not a command-not-found.
# The wording differs between the shells, so only the status is compared,
# and the definitions are NOT wrapped in a subshell -- that changes the
# parse context, which is a different question from this one.
echo "== refused"
$undefined107-bar() { ls; }
echo "dollar=$?"
foo-$(echo hi)() { ls; }
echo "cmdsub=$?"

echo end
