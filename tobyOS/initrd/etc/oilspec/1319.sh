f() { export GLOBAL=X; }
f
echo $GLOBAL
printenv.py GLOBAL
unset GLOBAL
echo g=$GLOBAL
printenv.py GLOBAL
