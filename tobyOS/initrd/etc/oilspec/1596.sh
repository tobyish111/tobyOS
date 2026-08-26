# Another invocation of _EvalWordToParts() that OSH should handle

shopt -s extglob
mkdir -p eg9
cd eg9
touch {foo,bar}.py
typeset -@(*.py) myvar
echo status=$?
