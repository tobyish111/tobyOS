case $SH in ash|dash) exit 99 ;; esac
a=(1 2 3)
a=v
argv.py "${a[@]}"
# Note: ash/dash does not have arrays
