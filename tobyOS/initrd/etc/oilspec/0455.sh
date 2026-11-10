case $SH in *dash) exit 99 ;; esac  # stderr unpredictable

foo=bar
typeset -p foo 1>&2

# zsh and mksh agree on exact output, which we don't really care about
