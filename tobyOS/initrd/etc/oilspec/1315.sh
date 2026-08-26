# Even after you do export -n, it still exists.
f() { export GLOBAL=X; }
f
echo $GLOBAL
printenv.py GLOBAL
