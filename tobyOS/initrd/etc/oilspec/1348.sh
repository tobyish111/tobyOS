case $SH in dash|mksh|zsh) return ;; esac

declare -A dict=()
key=1],a[1
dict["$key"]=foo
echo ${#dict[@]}
echo keys=${!dict[@]}
echo vals=${dict[@]}

unset -v 'dict["$key"]'
echo ${#dict[@]}
echo keys=${!dict[@]}
echo vals=${dict[@]}
