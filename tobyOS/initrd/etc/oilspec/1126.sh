case $SH in zsh) exit 99;; esac  # read -n not implemented

echo $'abc\\\ndefg' | (read -n 4; argv.py "$REPLY")

# mksh and ash implements "-n NUM" as number of bytes.
['abc']
