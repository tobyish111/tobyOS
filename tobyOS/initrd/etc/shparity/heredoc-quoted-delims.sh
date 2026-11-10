# A here-document delimiter quoted with a backslash, or written $'...' or
# $"...", is the word after quote removal, and the body is not expanded
# (batch 37: logs/b37/hdelim.sh). The backslash was kept: the reader looked
# for a line `\EOF` to the end of the script and expanded what it took.
x=X
read a b <<\EOF
$PS4 a
EOF
echo "[$a] [$b]"
cat <<\EOF
$x `echo no`
EOF
cat <<E\OF
$x mid
EOF
cat <<-\EOF
	$x tab
	EOF
read e <<\EOF
EOF
echo "[$e]"
cat <<\$E
dollar
$E
f() { cat <<\EOF
in f $x
EOF
}; f
cat <<$'A\tB'
$x tab-delim
A	B
cat <<$'E\'F'
$x quote-delim
E'F
cat <<$"LD"
$x locale-delim
LD
cat <<$'AB' | while read -r l; do echo "piped: $l"; done
$x
AB
echo end
