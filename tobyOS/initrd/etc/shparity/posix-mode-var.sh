# POSIXLY_CORRECT is posix mode: set, it turns the mode on, unset, off, as
# bash's does; and in posix mode an assignment before a special builtin
# stays after it (`x+=5 eval ...`). tsh ignored the variable and kept no
# prefix assignment in either mode (bash's appendop2.sub; finding 295).
x=2; x=9 eval 'echo "in=$x"'; echo "default after=$x"
POSIXLY_CORRECT=1
x=2; x+=5 eval 'printf "%s " "$x"'; echo "$x"
unset x; typeset -i x=2; x+=5 eval 'printf "%s " "$x"'; echo "$x"
a=1; a+=4; a+=5 eval 'printf "%s " "$a"'; echo "$a"
unset POSIXLY_CORRECT
y=2; y=9 eval :; echo "unset after=$y"
set -o posix; echo "set -o: [${POSIXLY_CORRECT-unset}]"
set +o posix; echo "set +o: [${POSIXLY_CORRECT-unset}]"
