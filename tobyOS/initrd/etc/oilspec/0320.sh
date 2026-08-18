# bash parses, but doesn't execute.
# mksh gives syntax error -- parses differently with 'export'
# osh no longer parses this statically.

export PYTHONPATH

PYTHONPATH=mystr  # NOTE: in bash, this doesn't work afterward!
printenv.py PYTHONPATH

PYTHONPATH=(myarray)
printenv.py PYTHONPATH

PYTHONPATH=(a b c)
printenv.py PYTHONPATH
