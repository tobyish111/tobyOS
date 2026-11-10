# A REDIRECTION THAT CANNOT BE OPENED is "FILE: Strerror" in bash, whatever
# the command -- tsh said "LABEL: 'FILE': lowercase" -- and the command does
# not run. (( )) takes redirections like any compound (tsh dropped them,
# and sent `(( 2 )) 2>f` to the subshell parser: 127). `echo x > /tmp` is
# EISDIR at the open (the kernel used to hand out a directory fd for
# writing). `x=$(< missing)` is status 1. And bash's arithmetic error
# keeps the expression's trailing blanks, once, with no second message
# from `for (( ))`; a failed subscripted assignment discards its line.
exec 2>&1
n=/nonexistent/d/f
if true; then echo no-if; fi > $n; echo "s1 $?"
{ echo no-group; } > $n; echo "s2 $?"
for i in 1; do echo no-for; done > $n; echo "s3 $?"
case x in x) echo no-case ;; esac > $n; echo "s4 $?"
while false; do :; done > $n; echo "s5 $?"
(( 1 )) > $n; echo "s6 $?"
[[ x ]] > $n; echo "s7 $?"
: > $n; echo "s8 $?"
f() { echo no-f; }; f > $n; echo "s9 $?"
echo no-echo > $n; echo "s10 $?"
cat < $n; echo "s11 $?"
/bin/echo no-ext > $n; echo "s12 $?"
: >> $n; echo "s13 $?"
: <> $n; echo "s14 $?"
echo x > /tmp; echo "s15 $?"
x=$(< $n); echo "s16 $? [$x]"
(echo no-sub) > $n; echo "s17 $?"
(( 2 )) 2> /dev/null; echo "s18 $?"
(( 0 )) > /dev/null; echo "s19 $?"
d=/tmp/rmsg; rm -rf $d; mkdir -p $d
(( x = 7 )) > $d/a; echo "s20 $? x=$x [$(cat $d/a)]"
(( 1/0 )) 2> $d/b; echo "s21 $?"; cat $d/b
rm -rf $d
(( 1/0 ))
((1/0))
((  1/0  ))
(( 1/0 )) 2>&1
for (( ; 1/0 ; )); do :; done
for ((;1/0;)); do :; done
for ((  i=1/0  ;;)); do :; done
for (( i=0; i<1; i+=1/0 )); do :; done
echo $(( 1/0 ))
echo $((1/0  ))
let ' 1/0 '
[[ ' 1/0 ' -eq 1 ]]
a[ 1/0 ]=x
a[ 1/0 ]=x
echo s=$?
b[1/0]=y; echo same
echo s=$?
c[1/0]=z
