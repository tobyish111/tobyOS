exec 2>&1
f() { local -x a=1; declare -p a; local -r b=2; declare -p b; local -i c=3+4; declare -p c; local -a d=(1 2); declare -p d; local -l e=ABC; declare -p e; local -u g=abc; declare -p g; local -xr h=5; declare -p h; local -x i; declare -p i; i=9; declare -p i; env | grep -c '^i=9'; }; f
f2() { declare -x a=1; declare -p a; typeset -x b=2; declare -p b; declare -gx c=3; }; f2; declare -p c
f3() { local a=1; export a; declare -p a; }; f3; declare -p a 2>&1
f4() { local +x PATH; declare -p PATH; }; f4
echo end
f5() { local -r x=1; }; f5; x=2; echo "x=$x"
f6() { declare -r y=1; }; f6; y=3; echo "y=$y"
f7() { local -q z 2>&1; echo "rc=$?"; }; f7
f8() { local -- -a 2>&1; echo "rc=$?"; local a -x 2>&1; echo "rc=$?"; }; f8
f9() { local +i n=3+4; declare -p n; local -i m=2; local +i m; m=1+1; declare -p m; }; f9
f10() { local -g G=5; }; f10; echo "G=$G"
echo end2
