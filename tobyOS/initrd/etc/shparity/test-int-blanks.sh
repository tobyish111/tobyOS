# test(1)'s integers, as bash reads them (legal_number): blanks -- spaces
# and tabs -- around the digits are allowed, a sign before them; anything
# else, an empty or blank-only operand and a number out of range is
# "integer expression expected", status 2. tsh took bare digits only, so a
# right-aligned number from `ps -o pid=` was a syntax error (batch 30).
t() { "$@" 2>&1; echo "rc=$?"; }
t test "  353" -eq 353
t test "353  " -eq 353
t test " 353 " -eq 353
t test "	7	" -eq 7
t [ "  3" -lt 4 ]
t [ 4 -gt " +3" ]
t test " -2" -lt 0
t test "+5" -eq 5
t test abc -eq 1
t [ 1 -eq abc ]
t test 1 -eq ""
t test " " -eq 0
t test "3 4" -eq 3
t test 99999999999999999999 -eq 1
t test 9223372036854775807 -eq 9223372036854775807
t test -9223372036854775808 -lt 0
t test 1 -eq 1 -a abc -lt 2
t test 1 -eq 1 -a " 2" -lt 3
t test 07 -eq 7
