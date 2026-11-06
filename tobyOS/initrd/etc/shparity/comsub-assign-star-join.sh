# A $( ) that bash starts while it expands a word it does not split -- an
# assignment's value, a case word, [[ ]], a here-string, a pattern -- runs
# its commands with that word's no-split flag: under a null IFS their
# unquoted "*" forms run together, and ${*OP} splits again at its spaces.
# An expansion in an assignment, a prefix assignment, a builtin's
# here-string, case and [[ ]] turn it off for the rest of the run; with
# IFS set nothing shows. tsh ran every $( ) as a fresh script (finding 610).
exec 2>&1
o() { printf '<%s>' "$@"; echo " $#"; }
set -- ax 'b cx' dx; arr=(ax 'b cx' dx); IFS=
x=$(o $*); echo "1 $x"
x=$(o ${*%x} ${*:2}); echo "2 $x"
x=$(o ${arr[*]} ${arr[*]%x}); echo "3 $x"
declare y=$(o $*); echo "4 $y"
x=${A-$(o $*)}; echo "5 $x"
x="${A-$(o $*)}"; echo "6 $x"
x=$(declare z=$*; echo "[$z]"); echo "7 $x"
x=$(o $*; q=1; o $*); echo "8 $x"
x=$(o $*; w=$IFS; o $*); echo "9 $x"
x=$(case a in a) ;; esac; o $*); echo "10 $x"
cat <<< $(o $*)
echo "12 $(o $*)"
x=$(g() { o $*; }; g; eval 'o $*'); echo "13 $x"
x=$(o $(o $*)); echo "14 $x"
IFS=:; x=$(o $*); echo "15 $x"
