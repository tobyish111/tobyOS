# `@` or `*` is no index of an indexed array: `ia[@]=x` is bash's "ia[@]:
# bad array subscript", an assignment error, the line dropped; an
# associative array's key @ is a key (finding 372). tsh's arithmetic said
# "@: syntax error: operand expected".
exec 2>&1
declare -a ia; ia[@]=garbage; echo "not here"
echo "rc=$?"; declare -p ia
a=(1); a[*]=x; echo "not here"
declare -p a
declare -A A; A[@]=at; declare -p A
