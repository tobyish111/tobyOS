case $SH in zsh) exit 99;; esac  # read -n not implemented

echo 'a\b\c\d\e\f' | (read -n 5; argv.py "$REPLY")
echo 'a\ \ \ \ \ ' | (read -n 5; argv.py "$REPLY")
