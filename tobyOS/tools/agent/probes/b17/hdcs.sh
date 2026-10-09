# hdcs.sh: a here-document whose operator is inside a $( ) that closes on
# the same line -- the body follows the LINE (finding 37). tsh warned, ran
# the body as a command and said "EOF: command not found".
exec 2>&1
x=$(cat <<EOF)
hello
EOF
echo "h1 [$x]"
echo "h2 [$(cat <<A) $(cat <<B)]"
a1
A
b1
B
y=$(cat <<-EOF)
	tabbed
	EOF
echo "h3 [$y]"
z=$(tr a-z A-Z <<EOF; echo after)
lower
EOF
echo "h4 [$z]"
v=$(cat <<'Q')
$HOME not expanded
Q
echo "h6 [$v]"
u=$(cat <<E1; cat <<E2)
one
E1
two
E2
echo "h7 [$u]"
echo "h8 [$(cat <<EOF
inside
EOF
)]"
t=`cat <<EOF`
back
EOF
echo "h9 [$t]"
# last: bash's own parser gives up here (unexpected EOF looking for `)')
cat <<OUT; w=$(cat <<IN)
outer
OUT
inner
IN
echo "h5 [$w]"
echo end
