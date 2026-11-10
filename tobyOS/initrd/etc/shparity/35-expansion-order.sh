# POSIX 2.6 -- the ORDER of expansions:
#   tilde -> parameter -> command substitution -> arithmetic
#   -> field splitting -> pathname expansion -> quote removal
# The order is observable: a parameter whose VALUE contains a glob character
# gets globbed (pathname expansion runs after parameter expansion), but a
# parameter whose value contains "$x" does NOT get re-expanded, because
# parameter expansion does not rerun on its own output.
count() { echo "n=$#"; }
> g1.txt
> g2.txt

pat='*.txt'
echo $pat
echo "$pat"
count $pat
count "$pat"

lit='$HOME'
echo $lit
echo "no-reexpand=$lit"

sp="a b"
count $sp
count "$sp"

n=3
echo "arith-after-param=$(( n + 1 ))"

both='* b'
count $both

HOME=/hd
tv=~
echo "tilde-in-value=$tv"
inval='~'
echo "tilde-not-reexpanded=$inval"

cs='$(echo hi)'
echo "cmdsubst-not-reexpanded=$cs"
echo end
