# A redirection that cannot be opened: bash says "FILE: Strerror" whatever
# the command is -- builtin, external, compound, function -- and the
# command does not run (status 1). (( )) and [[ ]] take redirections too.
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
