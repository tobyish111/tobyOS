case $SH in dash|mksh) exit 99 ;; esac # dash/mksh does not support associative arrays

declare -A dict
dict['foo']=hello
dict['bar']=oil
dict['baz']=world
declare -A dict
echo dict:${#dict[@]}
