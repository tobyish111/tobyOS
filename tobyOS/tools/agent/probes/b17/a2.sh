# a2.sh: the findings audit, builtins and expansion -- 10 26 (readonly in a
# function's compound assignment), 14 (pushd/popd +N -N), 16 ($BASHPID in
# ( ) and $( )), 17 (compgen), 18 (getconf logname pathchk), 42 (a
# multi-line arithmetic error), and two from memory: ${u:-'*'} globbing,
# and whether a -c shell waits for its background job at exit.
exec 2>&1
N() { sed 's/^[^:]*: line [0-9]*: //'; }
( readonly r=1; f() { local -a r=(9); }; f ) 2>&1 | N
( readonly r=1; g() { declare -a r=(9); }; g ) 2>&1 | N
( readonly r=1; h() { r=(9); }; h ) 2>&1 | N
cd /tmp; rm -rf ds; mkdir -p ds/a ds/b ds/c; cd ds
dirs -c; pushd a >/dev/null; pushd ../b >/dev/null; pushd ../c >/dev/null; dirs | sed "s#/tmp/ds#D#g"
pushd +1 >/dev/null; dirs | sed "s#/tmp/ds#D#g"
pushd -0 >/dev/null; dirs | sed "s#/tmp/ds#D#g"
popd +1 >/dev/null; dirs | sed "s#/tmp/ds#D#g"
popd -0 >/dev/null; dirs | sed "s#/tmp/ds#D#g"
pushd +7 2>&1 | N; popd -7 2>&1 | N
cd /tmp; rm -rf ds
a=$BASHPID; ( [ "$BASHPID" != "$a" ] && echo "f16 sub differs" || echo "f16 sub same" )
x=$(echo $BASHPID); [ "$x" != "$a" ] && echo "f16 cmdsub differs" || echo "f16 cmdsub same"
echo "f17 k=$(compgen -k | wc -l)"; compgen -A builtin ec; compgen -k fun; v1=1; compgen -v v1; alias al1=x; compgen -a al1
type getconf logname pathchk 2>&1 | sed 's#is /[^ ]*#is FILE#'
(( a = 3 + 4  # comment
   + ))
echo "f42 $?"
cd /tmp; rm -rf gq; mkdir gq; cd gq; touch a b; echo "g1" ${u:-'*'}; echo "g2" ${u:-"*"}; echo "g3" ${u:-*}; echo "g4" ${u:-\*}; cd /tmp; rm -rf gq
s=$SECONDS; $SH -c 'sleep 2 & echo started'; echo "fexit waited=$(( SECONDS - s ))"
echo end
