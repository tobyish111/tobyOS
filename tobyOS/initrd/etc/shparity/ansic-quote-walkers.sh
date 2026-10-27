# $'...' honours \' -- in every place tsh reads quotes ahead of the parser:
# brace expansion, [[ ]], <( ), (( )), for (( )), case pattern lists. Read
# the naive way the quote ended at the backslash (batch 37: logs/b37).
echo "== braces"
echo {p,$'q\'r'}s
echo {$'a\',b',c}x
echo x{a,$'b}c'}y
echo $'\'{a,b}'
echo $'\'{a,b}' {c,d}
echo $'a\' b' {x,y}
echo $'a\'b'{1..3}
echo {a,{$'b\'c',d}}
echo hi > $'f\'{a,b}'; read -r l < $'f\'{a,b}'; echo "$l"; rm -f $'f\'{a,b}'
x=$'a\'{1,2}'; echo "$x"
a=({$'a\'b',c}d); echo "${a[@]}" ${#a[@]}
declare -a d=({$'x\'y',z}); echo "${d[@]}"
echo "$'a\'" {x,y}
echo "{a,$'b'}" {c,d}
echo "== [[ ]]"
z=$'a\'b'; [[ $z == $'a\'b' ]] && echo yes || echo no
[[ $'a\'b|c' == *'|'* ]] && echo yes || echo no
[[ $'a]]b' == a* ]] && echo yes || echo no
[[ $(echo $'a\'b') == "a'b" ]] && echo yes || echo no
[[ "a'b" =~ $'a\'b' ]] && echo yes || echo no
[[ $'x\'' == "x'" && $'y\'' == "y'" ]] && echo yes || echo no
[[ -n $'\'' ]] && echo yes
echo "== <( )"
read -r l < <(echo $'ps\'ub'); echo "$l"
read -r l < <(echo $'a)b'); echo "$l"
read -r l < <(echo a\)b); echo "$l"
{ read -r l1; read -r l2 <&3; } < <(echo $'1\'') 3< <(echo $'2\''); echo "$l1 $l2"
echo "== (( ))"
z=$'a\'b'; (( ${#z} == 3 )) && echo yes || echo no
v=$(echo $'1\''); (( n = ${#v} + $(echo $'1\'' > /dev/null; echo 1) )); echo $n
echo $(( $(v=$'ab\''; echo ${#v}) + 1 ))
for (( i=0; i<$(v=$'2\''; echo ${#v}); i++ )); do printf '%s ' $i; done; echo
for (( i=0; i<2; i++ )); do echo $'i\'' $i; done
for (( i=0; i<1; i++ )) { echo $'b\'}' $i; }
echo "== case"
z=$'a\'b'; case $z in $'a\'b') echo yes;; *) echo no;; esac
case "x)" in $'x)') echo yes;; *) echo no;; esac
case "a'" in $'a\''|zz) echo yes;; *) echo no;; esac
case "a|b" in a\|b) echo yes;; *) echo no;; esac
case $'w\'' in "w'") echo yes;; *) echo no;; esac
case x in x) echo $'a;;b\'';; esac
echo "== the rest"
echo ${u:-$'x\'y z'}
echo ${u:-$'a}b'}
echo ${u:-$'a\'}b'}
z=$'a\'b'; echo ${z/$'\''/Q} ${z#$'a\''}
echo $'semi\'; echo not-a-command'
echo $(echo $(echo $'d\'p'))
f() { echo $'f\'n'; }; f
for w in $'a\'b' c; do echo "[$w]"; done
a=($'e\'f' g); echo ${#a[@]} "${a[0]}"
echo "== ( ) { } and the rest of the scans"
( echo $')\'' )
{ echo $'}\''; }
{ echo \}; }
g() ( echo $'g)\'' ); g
h() { echo $'}h\''; }; h
v=$( echo $')\'' ); echo "$v"
echo $'a\'' $(echo x)
w=$(< /dev/null); echo "[$w]"
declare -A m; m[$'k\'']=v; declare -p m
cat <<EOF; echo $'h\'d'
body
EOF
echo $'q\'' <<EOF
ignored body
EOF
echo \' <<EOF
ignored too
EOF
echo end
