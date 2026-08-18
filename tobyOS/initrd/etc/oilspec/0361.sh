case $SH in mksh) exit ;; esac

a=(x)
echo "[${a[-2]}]"
echo $?
echo "[$((a[-2]))]"
echo $?
