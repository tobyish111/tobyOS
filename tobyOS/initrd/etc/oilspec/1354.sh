case $SH in dash|zsh) exit ;; esac

a=(1 2 3)
unset a[-1]
echo len=${#a[@]}

echo last=${a[-1]}
(( last = a[-1] ))
echo last=$last

(( a[-1] = 42 ))
echo "${a[@]}"
