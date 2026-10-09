# THE QUOTED PART OF AN UNQUOTED ${x:-WORD} IS LITERAL to pathname
# expansion -- tsh globbed ${u:-'*'} -- and ${x:=WORD} stores and
# substitutes the plain value, as $x would give it; a :? message is the
# plain word (logs/b17 defword, gq, asg).
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
# ...in every place a word goes (logs/b17 defword2) -- not [[ ]] (finding 111) or a
# pattern inside double quotes (112), which were wrong before this and still are
cd /tmp; rm -rf dw2; mkdir dw2; cd dw2; touch a b fa fb
case 'a*' in ${u:-'a*'}) echo "c1 matched";; *) echo "c1 no";; esac
case ab in ${u:-'a*'}) echo "c2 matched";; *) echo "c2 no";; esac
case ab in ${u:-a*}) echo "c3 matched";; *) echo "c3 no";; esac
case ${u:-'*'} in '*') echo "c4 star";; *) echo "c4 other";; esac
[[ 'a*' == ${u:-'a*'} ]] && echo "d1 y" || echo "d1 n"
[[ ab == ${u:-a*} ]] && echo "d3 y" || echo "d3 n"
echo hi > ${u:-'f*'}; ls; rm -f 'f*'
cat <<< ${u:-'q*'}
for i in ${u:-'*'} ${u:-"?"}; do echo "f [$i]"; done
arr=(${u:-'*'} ${u:-*}); echo "a ${#arr[@]} ${arr[0]}"
f() { local l=${u:-'*'}; echo "l [$l]"; }; f
export e=${u:-'*'}; echo "e [$e]"
echo "n1" ${u:-${w:-'*'}} ${u:-${w:-*}}
echo "n2" ${u:-'['ab']'} ${u:-x'*'y}
echo "n3" ${a+'*'} ${a+*}
a=set; echo "n4" ${a:+'f*'} ${a:+f*}
cd /tmp; rm -rf dw2
# a word with literal glob characters AND an expansion still globs (Oils 1583 1703:
# escaping in the funnel stopped it; logs/b17 globreg)
cd /tmp; rm -rf gr; mkdir gr; cd gr
mkdir x; touch 'x/test.ifs.\.txt' 'x/test.ifs.*.txt'
v='*\*.txt'; echo 1 x/$v
v="\\" u='*.txt'; echo 2 x/*$v$u
v="\\" u="*.txt"; echo 3 x/*$v*.txt
shopt -s extglob
touch {foo,bar}.cc {foo,bar,baz}.h foo. foo.hh
ext=cc; echo 4 foo.?($ext|h)
echo 5 foo.+($ext|h)
a=(x); echo 6 @(fo|ba)*.$ext
cd /tmp; rm -rf gr
echo end
