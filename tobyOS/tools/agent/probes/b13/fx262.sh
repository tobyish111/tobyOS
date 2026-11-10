d=/tmp/fxd.$$; rm -rf $d; mkdir $d; cd $d
# [[ ]] EXPANDS AN OPERAND WHEN IT IS EVALUATED (batch 13), as bash does:
# the side of && or || that does not decide is not expanded at all, so its
# substitutions do not run and its ${v:=x} assigns nothing. tsh expanded
# every operand before evaluating any. (logs/b13 q35)
[[ -n x || $(echo ran1; touch f1) ]]; echo "rc=$? f1=$([ -e f1 ] && echo yes || echo no)"
[[ -z x && $(touch f2) == y ]]; echo "rc=$? f2=$([ -e f2 ] && echo yes || echo no)"
[[ a == a || $((n1=5)) ]]; echo "n1=${n1-unset}"
[[ a == b && ${n2:=7} ]]; echo "n2=${n2-unset}"
[[ ( a == b && ${n3:=1} ) || ${n4:=2} ]]; echo "n3=${n3-unset} n4=${n4-unset}"
[[ ! ( -z x && ${n5:=3} ) ]]; echo "rc=$? n5=${n5-unset}"
v=-f; [[ $v == -f ]] && echo literal-op
x='<'; [[ -n $x ]] && echo nonempty
echo end
