# trap's options are -l and -p; any other letter is "trap: -s: invalid
# option" and bash's usage line, status 2 -- tsh read `trap -p -s NOSIG`'s
# -s as a signal (finding 421).
exec 2>&1
trap -p -s NOSIG; echo "rc=$?"; trap -x; echo "rc=$?"; trap -lx; echo "rc=$?"
trap 'echo t' USR1; trap -p USR1; trap -- - USR1; trap -p USR1; echo "rc=$?"
