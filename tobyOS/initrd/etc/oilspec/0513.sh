case $SH in ash|dash)  exit 99 ;; esac
a=typeset
v=x
"$a" v=1
echo "v=$v"
