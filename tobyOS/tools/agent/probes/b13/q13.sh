S=${SH:-bash}; S=$(command -v $S)
echo "== 1"; env -i $S -c 'env'
echo "== 2"; env -i $S -c 'cd /tmp; env'
echo "== 3"; env -i $S -c 'env >/dev/null; cd /tmp; env'
echo "== 4"; env -i A=1 B=2 C=3 $S -c 'X=9 Y=8 env'
echo "== 5"; env -i A=1 $S -c 'f() { local L1=1 L2=2; export L1 L2; env; }; f'
echo "== 6"; env -i A=1 $S -c 'f() { local -x a1=1 a2=2 a3=3 a4=4 a5=5 a6=6 a7=7 a8=8 a9=9; env; }; f'
echo "== 7"; env -i A=1 $S -c 'f() { env; }; P=1 Q=2 f'
echo "== 8"; env -i A=1 $S -c 'f() { local -x L=1; g; }; g() { local -x M=2; env; }; f'
echo "== 9"; env -i $S -c 'echo "PATH=[$PATH] HOME=[${HOME-unset}] USER=[${USER-unset}] SHELL=[${SHELL-unset}] TERM=[${TERM-unset}]"; declare -p PATH SHLVL 2>&1; export -p'
echo "== 10"; env -i SHLVL=5 $S -c 'echo $SHLVL; env | grep SHLVL'
echo "== 11"; env -i $S -c 'unset PWD; env; export Z=1; env'
echo "== 12"; env -i $S -c 'B=1; export B; A=2; export A; env; unset B; export B=3; env'
echo end
