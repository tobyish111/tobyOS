exec 2>&1 </dev/null
cd /tmp
echo "== simple"
set -x
echo hi
echo 'a b' "" $'\t' "it's" '*' a=b '~' '#x' 'x#' 'a|b' '$x' 'q"q' "back\\slash" '{a}' '!' x%y
echo $'a\nb' $'\001' "$(printf '\303\251')"
: ; true ; test 1 = 1 ; [ 1 = 1 ]
set +x
echo "== assignments"
set -x
x=1
x='a b'
x=
y="*"
z=$'p\nq'
a=(1 2 'x y')
a+=(3)
a[1]=zz
x+=more
n=$((1+2))
x=1 true
x=1 y=2 echo pre
declare d=2
export e=3
readonly r1=4
declare -a arr=(1 2)
local_t() { local v=1; local w; }
local_t
set +x
echo "== compound"
set -x
for i in a b; do :; done
l='p q'; for i in $l; do :; done
for ((i=0; i<2; i++)); do :; done
case xyz in x*) : ;; esac
v=abc; case $v in abc) : ;; esac
[[ a == b ]]
[[ -n $v ]]
[[ $v ]]
[[ a =~ ^a ]]
[[ ! -z x && 1 -lt 2 ]]
(( 1 + 2 ))
(( n = n + 1 ))
if true; then :; fi
while false; do :; done
! false
{ echo grp; }
( echo sub )
set +x
echo "== levels"
set -x
f() { echo in-f; }
f
c=$(echo comsub)
c=$(echo $(echo deep))
eval 'echo ev'
eval "eval 'echo ev2'"
printf 'echo sourced\n' > /tmp/q30src.sh
. /tmp/q30src.sh
trap 'echo trapped' USR1
kill -USR1 $$
trap - USR1
echo "$(echo inner)" >/dev/null
set +x
echo "== ps4"
PS4='>> '; set -x; echo p1; c=$(echo p2); set +x
PS4='+$LINENO: '; set -x; echo p3; set +x
PS4=''; set -x; echo p4; set +x
PS4='$(echo X) '; set -x; echo p5; set +x
PS4='é '; set -x; c=$(echo p6); set +x
PS4=$'é '; set -x; c=$(echo p6b); set +x
PS4='+ '
echo "== xtracefd"
exec 9>/tmp/q30xt
BASH_XTRACEFD=9
set -x
echo tofd
set +x
unset BASH_XTRACEFD
exec 9>&-
echo "fd got:"; cat /tmp/q30xt
echo "== misc"
set -x
echo hi >/dev/null
cat <<EOF
heredoc
EOF
empty=; $empty
alias ll='echo aliased'
shopt -s expand_aliases
set +x
echo end
