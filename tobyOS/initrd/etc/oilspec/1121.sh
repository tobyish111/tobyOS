case $SH in dash|ash) return ;; esac  # not implemented

# same hanging bug
case $SH in mksh) return ;; esac

mkdir -p dir
read -n 3 x < ./dir
echo status=$?
