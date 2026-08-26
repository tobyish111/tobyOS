typeset s='abc'
echo $s

typeset s+=(d e f)
echo status=$?
argv.py "${s[@]}"
