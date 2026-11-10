# An expansion error in a case pattern ends the case, status 1, and the rest
# of the line with it -- `case 1 in $((xx++)) )` of a readonly xx runs no
# clause; tsh went on to the `*)` clause (finding 437).
exec 2>&1
readonly xx=1
case 1 in $((xx++)) ) echo hi1 ;; *) echo hi2; esac; echo "same line"
echo "${xx}.$?"
case 2 in $((1/0)) ) echo a ;; *) echo b ;; esac
echo "rc=$?"
