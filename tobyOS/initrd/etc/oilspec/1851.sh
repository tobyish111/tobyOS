x=foo
typeset -n -x ref=x

# hm bash ignores it but mksh doesn't.  maybe disallow it.
printenv.py x ref
echo ---
export x
printenv.py x ref
