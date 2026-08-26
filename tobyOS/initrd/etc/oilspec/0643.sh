# Relies on timeout from GNU coreutils
timeout 0.5 $SH -c 'cat /dev/zero | cat | head -c 5 | tr \\0 0; echo'

# The exact cause of the issue is:
#
#  $ cat /dev/zero | cat | head -c 5
#
# A pipe through cat is necessary and the first cat MUST be reading from a
# character device.

# Other cases without the bug above
printf '\0\0\0\0\0\0\0' > tmp.txt
$SH -c 'cat </dev/zero | head -c 5 | tr \\0 0; echo'
$SH -c 'cat /dev/zero | head -c 5 | tr \\0 0; echo'
$SH -c 'cat tmp.txt | cat | head -c 5 | tr \\0 0; echo'
