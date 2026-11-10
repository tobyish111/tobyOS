case $SH in dash|ash|mksh|zsh) return ;; esac  # not implemented

mkdir -p dir
mapfile $x < ./dir
echo status=$?
