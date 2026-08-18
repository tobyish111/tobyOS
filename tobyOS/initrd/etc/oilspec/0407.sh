code='x=(1 2 3)'
typeset -a "$code"  # note: -a flag is required
echo status=$?
argv.py "$x"
# bash allows it
