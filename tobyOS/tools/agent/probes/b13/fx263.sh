d=/tmp/fxd.$$; rm -rf $d; mkdir $d; cd $d
# set -x AS BASH PRINTS IT (batch 13). tsh printed "+ " and a pipeline's
# words as they stood, joined by "| ". bash: PS4 expanded with its first
# character repeated per level of nesting (eval, `.`, $( ), a trap); a
# word quoted as input would need it; assignments a line each, an array
# literal re-spaced as written; for/case heads as written, `(( ))` and the
# three parts of `for (( ))` expanded, each [[ ]] test as it is evaluated;
# declare/local/export/readonly split a compound argument out; and
# BASH_XTRACEFD. (logs/b13 q30 q34)
exec 2>&1
set -x
echo 'a b' '' "it's" '*' a=b '~' '#x' x# 'a|b' '$x' "$(printf 'ok')" x%y
x='a b'; y=; z=$'p\tq'
a=(  1   2  'x y' )
a[1]=zz
x=1 true
n=$((1+2))
for i in a "b c"; do :; done
set -- p q; for i; do :; done
for (( i = 0 ; i < 1 ; i++ )); do :; done
for ((;;)); do break; done
v=abc; case $v in abc) : ;; esac
[[ a == "*" && -n $v ]]
[[ a == 'b c' || ! -z x ]]
[[ $v ]]
[[ a =~ "x.y" ]]
(( $n + 1 ))
c=$(echo $(echo deep))
eval 'echo ev'
f() { local -a la=(1 2); declare -A m=([k]=v); export e=3 f2; readonly r1=4; }
f
PS4='>> '; c=$(echo sub)
PS4=''; echo nops4
PS4='+$x: '; echo vars
PS4='+ '
exec 9>xt.log; BASH_XTRACEFD=9; echo tofd; unset BASH_XTRACEFD; exec 9>&-
set +x
echo "fd:"; cat xt.log
echo end
