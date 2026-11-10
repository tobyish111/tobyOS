case $SH in mksh) exit 99 ;; esac
declare -a a=([xx]=1 [yy]=2 [zz]=3)
echo status=$?
argv.py "${a[@]}"
