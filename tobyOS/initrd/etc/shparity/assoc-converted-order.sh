# An associative array made from a scalar has bash's smaller hash table,
# and a list assigned to it later keeps that table: its keys list in that
# table's order, not in the order of one made by `declare -A` (finding
# 386). tsh used declare -A's table for both.
exec 2>&1
foo=help; declare -A foo=([zero]=zero [one]=one); declare -p foo
unset foo; declare -A foo=([zero]=zero [one]=one); declare -p foo
bar=x; declare -A bar; declare -p bar; bar=([zero]=zero [one]=one); declare -p bar
f() { declare -gA baz=([zero]=zero [one]=one); }; baz=help; f; declare -p baz
