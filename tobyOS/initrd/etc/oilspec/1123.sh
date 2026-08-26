case $SH in zsh) exit 99;; esac  # read -n not implemented

echo 'a\b\c\d\e\f' | (read -n 0; argv.py "$REPLY")

# ash appears to treat 0 as unspecified
