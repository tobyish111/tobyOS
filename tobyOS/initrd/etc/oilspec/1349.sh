case $SH in dash|mksh) return ;; esac

declare -A assoc=(['key']=value)
unset 'assoc["nonexistent"]'
echo status=$?
