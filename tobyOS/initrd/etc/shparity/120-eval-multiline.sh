eval 'cat <<A
E01 heredoc
A'
eval 'if true
then echo E02 if
fi'
f3() { eval 'return 3'; echo E03 not-reached; }; f3; echo "E03 rc=$?"
f4() { eval 'echo E04 a
return 4
echo E04 b'; echo E04 not-reached; }; f4; echo "E04 rc=$?"
shopt -s expand_aliases
eval 'alias sayhi="echo E05 hi"
sayhi'
f6() { v=call; eval 'cat <<A
E06 $v
A'; }; v=def; f6
for i in 1 2; do eval 'cat <<A
E07 it $i
A
break'; done
eval 'x=1
y=2'; echo "E08 $x$y"
eval 'echo "E09 $?"
false
echo "E09 after $?"'
eval 'echo E10 a
if then
echo E10 c'; echo "E10 rc=$?"
eval 'eval "cat <<B
E11 nested
B"'
( eval 'echo E12 in
exit 5
echo E12 no' ); echo "E12 rc=$?"
f13() { eval 'cat <<A
E13 in fn
A'; echo E13 after; }; f13; f13
eval 'for j in a b
do echo E14 $j
done'
eval 'case x in
x) echo E15 arm;;
esac'
eval 'echo E16 one; echo E16 two'
f17() { eval 'return 7'; }; f17; echo "E17 rc=$?"
shopt -s expand_aliases
alias hey='echo E18 hey'
eval hey
eval "if then"; echo "E19 st=$?"
v=$(eval 'if true
then echo E21 sub
fi'); echo "$v"
( eval 'echo E22 a
echo ${nope:?E22 boom}
echo E22 c'; echo E22 no ); echo "E22 rc=$?"
( eval 'echo E23 a
exit 6
echo E23 no'; echo E23 not-reached ); echo "E23 rc=$?"
for i in 1 2 3; do eval 'echo E24 $i
[ $i = 2 ] && continue
echo E24 post $i'; done
echo done
