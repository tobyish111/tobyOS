case $SH in dash) exit ;; esac

#shopt -s strict_arith

# This overflows - the extra 9 puts it above 2**31
#echo $(( 12345678909 ))

[[ 12345678909 = $(( 1 << 30 )) ]]
echo eq=$?
[[ 12345678909 = 12345678909 ]]
echo eq=$?

# Try both [ and [[
[ 12345678909 -gt $(( 1 << 30 )) ]
echo greater=$?
[[ 12345678909 -gt $(( 1 << 30 )) ]]
echo greater=$?

[[ 12345678909 -ge $(( 1 << 30 )) ]]
echo ge=$?
[[ 12345678909 -ge 12345678909 ]]
echo ge=$?

[[ 12345678909 -le $(( 1 << 30 )) ]]
echo le=$?
[[ 12345678909 -le 12345678909 ]]
echo le=$?


# mksh still uses int!
