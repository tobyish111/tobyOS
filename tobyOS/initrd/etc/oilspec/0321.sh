shopt -s strict_array

export PYTHONPATH
PYTHONPATH=(a b c)
printenv.py PYTHONPATH
