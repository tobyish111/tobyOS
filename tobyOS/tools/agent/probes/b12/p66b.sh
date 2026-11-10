exec 2>&1
p=$BASHPID
x=$(echo $BASHPID); [ "$x" != "$p" ] && echo "c1 differs"
( [ "$BASHPID" != "$p" ] && echo "s1 differs" )
[ "$$" = "$p" ] && echo "top same"
x=$(echo $$); [ "$x" = "$p" ] && echo "c2 dollar same"
x=$(trap 'echo bye' EXIT; echo hi); echo "[$x]"
( trap 'echo sbye' EXIT; echo shi )
x=$(exit 7); echo "st=$?"
( exit 9 ); echo "st=$?"
x=$(y=5; cd /; echo in); echo "[$x] y=${y-unset} pwd=$PWD"
( z=1; cd /tmp ); echo "z=${z-unset} pwd=$PWD"
x=$(echo a; echo b >&2; echo c); echo "[$x]"
x=$(printf 'a\0b'); echo "len=${#x}"
x=$( for i in 1 2 3; do echo $i; done ); echo "[$x]"
x=$(cat <<EOF
here $p
EOF
); [ "$x" = "here $p" ] && echo "heredoc ok"
cat <<A; x=$(echo mid); cat <<B
first
A
second $x
B
( cat <<C
inner
C
); cat <<D
outer
D
trap 'echo parent-exit' EXIT
x=$(echo nested $(echo deep $(echo deeper))); echo "[$x]"
f() { echo fn; return 3; }
x=$(f); echo "[$x] $?"
set -e
x=$(false; echo after); echo "[$x] e"
echo $( (exit 4) ; echo "in $?" )
v=$( (echo sub; exit 2) ); echo "[$v] $?"
echo done
