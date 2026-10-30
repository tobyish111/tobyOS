# unset of an element of an array only declared (declare -a AA) is "[-2]:
# bad array subscript" for an index still negative, as for an empty array,
# and nothing for any other -- not "AA: not an array variable" (finding 415).
exec 2>&1
declare -a AA; unset AA[-2]; echo "rc=$?"; unset AA[3]; echo "rc=$?"; declare -p AA
BB=(); unset BB[-1]; echo "rc=$?"
s=x; unset s[1]; echo "rc=$?"
