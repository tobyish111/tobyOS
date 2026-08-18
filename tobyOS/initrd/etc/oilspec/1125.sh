case $SH in zsh) exit 99;; esac  # read -n not implemented

echo 'abc\def\ghijklmn' | (read -n 4; argv.py "$REPLY")
echo '   \xxx\xxxxxxxx' | (read -n 4; argv.py "$REPLY")

# bash implements "-n NUM" as number of characters
# ash implements "-n NUM" as number of bytes
['abc']
['   ']
# mksh implements "-n NUM" as number of bytes, and also "read" (without
# variable names) in mksh is equivalent to "read REPLY, i.e., consideres IFS.
['abc']
['']
