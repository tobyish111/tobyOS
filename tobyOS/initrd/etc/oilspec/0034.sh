shopt -s expand_aliases
alias p_=printenv.py
FOO=1 printenv.py FOO
FOO=2 p_ FOO
