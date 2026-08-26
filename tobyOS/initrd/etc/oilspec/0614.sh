# zsh does something crazy like : ; < = > that I'm not writing
case $SH in *zsh) echo BUG; exit ;; esac
echo {1..a}
echo {z..3}
