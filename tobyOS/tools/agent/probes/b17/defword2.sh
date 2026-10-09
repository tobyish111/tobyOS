# defword2.sh: the quoted part of a ${x:-WORD} in every place a word goes
# -- a case word and pattern, [[ ]], a redirection, a here-string, for, an
# array literal, local and export, ${v#pat}, ${v/pat/rep}, nested -- and
# what reaches the command: literal where it was quoted, a pattern where
# not (finding 110's fix moved from the funnel to the word, logs/b17).
exec 2>&1
cd /tmp; rm -rf dw2; mkdir dw2; cd dw2; touch a b fa fb
case 'a*' in ${u:-'a*'}) echo "c1 matched";; *) echo "c1 no";; esac
case ab in ${u:-'a*'}) echo "c2 matched";; *) echo "c2 no";; esac
case ab in ${u:-a*}) echo "c3 matched";; *) echo "c3 no";; esac
case ${u:-'*'} in '*') echo "c4 star";; *) echo "c4 other";; esac
[[ 'a*' == ${u:-'a*'} ]] && echo "d1 y" || echo "d1 n"
[[ ab == ${u:-'a*'} ]] && echo "d2 y" || echo "d2 n"
[[ ab == ${u:-a*} ]] && echo "d3 y" || echo "d3 n"
echo hi > ${u:-'f*'}; ls; rm -f 'f*'
cat <<< ${u:-'q*'}
for i in ${u:-'*'} ${u:-"?"}; do echo "f [$i]"; done
arr=(${u:-'*'} ${u:-*}); echo "a ${#arr[@]} ${arr[0]}"
f() { local l=${u:-'*'}; echo "l [$l]"; }; f
export e=${u:-'*'}; echo "e [$e]"
v='a*b'; echo "p1 ${v#${u:-'a*'}}" "p2 ${v#${u:-a*}}" "p3 ${v/${u:-'*'}/X}"
echo "n1" ${u:-${w:-'*'}} ${u:-${w:-*}}
echo "n2" ${u:-'['ab']'} ${u:-x'*'y}
echo "n3" ${a+'*'} ${a+*}
a=set; echo "n4" ${a:+'f*'} ${a:+f*}
cd /tmp; rm -rf dw2; echo end
