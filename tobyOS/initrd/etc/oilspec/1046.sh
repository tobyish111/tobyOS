case $SH in zsh) exit ;; esac

# leading space is allowed
printf '%d\n' ' -123'
echo status=$?
printf '%d\n' ' -123 '
echo status=$?

echo ---

printf '%d\n' ' +077'
echo status=$?

printf '%d\n' ' +0xff'
echo status=$?

printf '%X\n' ' +0xff'
echo status=$?

printf '%x\n' ' +0xff'
echo status=$?
