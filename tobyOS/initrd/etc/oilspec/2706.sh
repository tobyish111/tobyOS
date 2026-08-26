# This behavior is weird, but all shells agree.
IFS='_ '
s1='_ a  b _ '
s2='  a  b _ '
argv.py $s1
argv.py $s2
