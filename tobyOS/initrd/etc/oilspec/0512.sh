case $SH in ash|dash)  exit 99 ;; esac
a=(typeset v=1)
v=x
"${a[@]}"
echo "v=$v"
# Note: ash/dash does not have arrays
