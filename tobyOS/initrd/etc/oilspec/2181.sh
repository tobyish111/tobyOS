case $SH in zsh) return ;; esac  # zsh doesn't make much sense

zz=$'one\ntwo'

set | grep zz
