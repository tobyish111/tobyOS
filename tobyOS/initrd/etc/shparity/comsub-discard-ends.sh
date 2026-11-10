# A command substitution is a subshell, and an expansion error that
# discards a line ends a subshell: whatever it printed before stays, the
# lines after the error do not run, and its status is 1. A substitution
# over several lines ran its next line in tsh (finding 628).
exec 2>&1
x=$(x=$((1+)); echo a
echo b); echo "[$x] $?"
x=$(echo first
echo ${a[}; echo not
echo nor); echo "[$x] $?"
x=$(echo one; y=$((2/0))
echo two); echo "[$x] $?"
x=$(echo ok
echo fine); echo "[$x] $?"
