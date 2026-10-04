export ZED=1 ALPHA=2 mid=3 B_2=4 a=5 LONGERNAME_X=6 Q=7
f() { echo f; }; g() { echo g; }; abc() { :; }
export -f f g abc
env | grep -v '^_=' | grep -E '^(ZED|ALPHA|mid|B_2|a|LONGERNAME_X|Q|BASH_FUNC_)'
echo ---
X1=a X2=b env | grep -E '^(X1|X2|ZED|Q)='
echo ---
env -i K3=1 K1=2 K2=3 "$BASH" -c 'export N1=1 N0=2; env' | grep -v '^_=\|^PWD=\|^SHLVL=\|^OLDPWD='
