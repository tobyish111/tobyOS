# POSIX readonly: assigning or unsetting a readonly variable must FAIL
# without killing a non-interactive shell, and -p prints a reinput form.
readonly RO=locked
RO=changed
echo "1=$?:$RO"
unset RO
echo "2=$?:$RO"
readonly -p | grep 'RO='
export EXP_V=val
export -p | grep 'EXP_V='
unset -v NEVER_SET_V
echo "4=$?"
f() { echo fn; }
f
unset -f f
f
echo "5=$?"
echo end
