case $SH in ash|zsh) return ;; esac  # zsh doesn't make much sense

zz=$'one\ntwo'

typeset | grep zz
typeset -p zz
