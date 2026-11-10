# Functions, sourced files and eval nested at most 8 deep. bash has no limit
# of its own (the stack is the limit), and FUNCNEST sets one.
h() { [ $1 -gt 0 ] && h $(($1 - 1)); :; }; h 300; echo "r1 $?"
d=0; k() { d=$((d+1)); [ $d -lt 1000 ] && k; }; k; echo "r2 $d"
printf '%s\n' 'm=$1; [ $m -gt 0 ] && . ./nest_src.sh $((m-1)); s=$((s+1))' > nest_src.sh
s=0; . ./nest_src.sh 100; echo "r3 $s"
x='c=$((c+1)); [ $c -lt 100 ] && eval "$x"'; c=0; eval "$x"; echo "r4 $c"
FUNCNEST=5; n=0; f() { n=$((n+1)); f; }; f; echo "r5 not-reached"
echo "r6 $? n=$n"
n=0; g() { n=$((n+1)); [ $n -lt 5 ] && g; }; g; echo "r7 $? n=$n"
n=0; x=$(f; echo in-subst); echo "r8 [$x] $?"
n=0; ( f; echo in-subshell ); echo "r9 $?"
n=0; eval f; echo "r10 $? n=$n"
n=0; w() { f; echo after-f; }; w; echo "r11 not-reached"
echo "r12 $? n=$n"
FUNCNEST=' 3 '; n=0; f; echo "r13 not-reached"
echo "r14 $? n=$n"
for v in abc 0 -1 ''; do FUNCNEST=$v; n=0; z() { n=$((n+1)); [ $n -lt 20 ] && z; }; z; echo "r15 [$v] $n"; done
unset FUNCNEST
rm -f nest_src.sh
echo done
