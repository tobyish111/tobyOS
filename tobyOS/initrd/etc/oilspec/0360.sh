case $SH in mksh) exit ;; esac

a[0]=x
a[2]=y
echo "quoted = (${a[@]@Q})"
