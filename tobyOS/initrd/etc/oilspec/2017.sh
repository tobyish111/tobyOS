PS1='foo \l bar'
# FIXME this never an actual TTY when using ./test/spec.sh
tty="$(tty)"
if [[ "$tty" == "not a tty" ]]; then
    expected="tty"
else
    expected="$(basename "$tty")"
fi
echo "${PS1@P}" | egrep -q "foo $expected bar"
echo matched=$?
