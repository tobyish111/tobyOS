case $SH in mksh) exit ;; esac

sp=({1..9})
unset -v 'sp[2]'
unset -v 'sp[3]'
unset -v 'sp[7]'

echo "sp[0]: '${sp[0]}', ${sp[0]:-(empty)}, ${sp[0]+set}."
echo "sp[1]: '${sp[1]}', ${sp[1]:-(empty)}, ${sp[1]+set}."
echo "sp[8]: '${sp[8]}', ${sp[8]:-(empty)}, ${sp[8]+set}."
echo "sp[2]: '${sp[2]}', ${sp[2]:-(empty)}, ${sp[2]+set}."
echo "sp[3]: '${sp[3]}', ${sp[3]:-(empty)}, ${sp[3]+set}."
echo "sp[7]: '${sp[7]}', ${sp[7]:-(empty)}, ${sp[7]+set}."

echo "sp[-1]: '${sp[-1]}'."
echo "sp[-2]: '${sp[-2]}'."
echo "sp[-3]: '${sp[-3]}'."
echo "sp[-4]: '${sp[-4]}'."
echo "sp[-9]: '${sp[-9]}'."
