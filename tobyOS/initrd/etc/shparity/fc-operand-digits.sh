# An fc operand is a number when it starts with a digit, after its `-`,
# and the number is its leading digits: `1x` is 1, `0x1` 0, while `+1`
# and ` 1` are strings to look for, and `-1z` is options -- tsh took only
# an operand all digits for a number (finding 549).
exec 2>&1
unset HISTFILE
set -o history
echo a
echo b
echo c
fc -l 1x 2y
fc -l 0x1
fc -l -1z
fc -l +1; echo "rc=$?"
fc -l ' 1'; echo "rc=$?"
