case $SH in mksh) exit ;; esac

sp=({1..9})
unset -v 'sp[2]'
unset -v 'sp[3]'
unset -v 'sp[7]'

echo "sp[-10]: '${sp[-10]}'."
echo "sp[-11]: '${sp[-11]}'."
echo "sp[-19]: '${sp[-19]}'."
