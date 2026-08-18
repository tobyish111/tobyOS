case $SH in mksh) exit ;; esac

v1=hello v2=world
a=(v1 v2)

echo "${!a[0]}, ${!a[1]}"
