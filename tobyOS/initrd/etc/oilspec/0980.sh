# There are variations on whether quotes are printed

case $SH in dash|zsh) return ;; esac

foo=spam

set | grep -A1 foo

# Will print multi-line and binary data literally!
typeset -p foo

printf 'pf  %q\n' "$foo"

echo '@Q ' ${foo@Q}
