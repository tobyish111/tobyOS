case $SH in dash|zsh) exit ;; esac

a=(0 1 2 3 4)
unset a[1]
unset a[4]
echo len=${#a[@]} a=${a[@]}
echo last=${a[-1]} second=${a[-2]} third=${a[-3]}

echo ---
unset a[3]
echo len=${#a[@]} a=${a[@]}
echo last=${a[-1]} second=${a[-2]} third=${a[-3]}
