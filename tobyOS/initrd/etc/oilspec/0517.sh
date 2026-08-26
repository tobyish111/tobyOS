case $SH in ash|dash|yash) exit 99 ;; esac
a=()

a[1]=x
eval 'a[5&3]=hello'
echo "status=$?, a[1]=${a[1]}"

a[2]=x
eval 'a[1 + 1]=hello'
echo "status=$?, a[2]=${a[2]}"

a[3]=x
eval 'a[1|2]=hello'
echo "status=$?, a[3]=${a[3]}"
# Note: ash/dash does not have arrays
# Note: yash does not support a[index]=value
