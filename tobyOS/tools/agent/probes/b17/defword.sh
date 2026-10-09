# defword.sh: the quoted part of an unquoted ${x:-WORD} is literal for
# pathname expansion (tsh globbed '*'); ${x:=WORD} stores the plain
# value and substitutes it as $x would; a :? message is the plain word.
exec 2>&1
cd /tmp; rm -rf gq; mkdir gq; cd gq; touch a b
echo "g1" ${u:-'*'}; echo "g2" ${u:-"*"}; echo "g3" ${u:-*}; echo "g4" ${u:-\*}
echo "g5" "${u:-'*'}" "${u:-"*"}"
v='*'; echo "g6" ${u:-"$v"} ${u:-$v}
: ${w:='a*'}; echo "g7 [$w]"; : ${w2:="b?"}; echo "g8 [$w2]"
echo "g9" ${u+'*'} ${a+'?'}; a=1; echo "g10" ${a:+'*'} ${a:+*}
x=${u:-'*'}; echo "g11 $x"
( : ${q:?'msg*'} ) 2>&1 | sed 's/^.*: q:/q:/'
case ab in ${u:-'a*'}) echo "g12 matched";; *) echo "g12 no";; esac
case 'a*' in ${u:-'a*'}) echo "g13 matched";; *) echo "g13 no";; esac
echo "g14" ${u:-'[ab]'} ${u:-[ab]}
cd /tmp; rm -rf gq; echo end
cd /tmp; rm -rf asg; mkdir asg; cd asg; touch q1 q2
unset w; printf '[%s]' ${w:='a  b'}; echo " w=[$w]"
unset v; echo ${v:='q*'} "v=[$v]"
unset x; echo ${x:=q*} "x=[$x]"
unset y; printf '[%s]' "${y:='a  b'}"; echo " y=[$y]"
unset z; : ${z:=\*}; echo "z=[$z]"
cd /tmp; rm -rf asg
