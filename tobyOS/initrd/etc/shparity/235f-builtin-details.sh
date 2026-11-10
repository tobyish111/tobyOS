# Brace ranges across letters, readonly in posix mode, the ERR trap for a
# failing ( ), unset command names, alias -p/--, readonly -p/export -p.
# (batch 12; probe p47 in C:\t\b12)
exec 2>&1
alias -- foo=echo; echo "1 $?"; alias -p | grep foo; alias -x; echo "2 $?"
alias -p -- foo; echo "3 $?"
unalias -- foo; echo "4 $?"; unalias -a; echo "5 $?"; unalias -z; echo "6 $?"
alias -- -x=y; echo "7 $?"; alias -- -x; unalias -- -x; echo "8 $?"
alias; echo "9 $?"
alias ''; echo "10 $?"
alias 'a b=c'; echo "11 $?"
