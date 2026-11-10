# mksh's readonly does not support the -a option.
# dash/mksh does not support associative arrays.
case $SH in dash|mksh) exit 99 ;; esac

declare -a arr
arr=(foo bar baz)
declare -A dict
dict['foo']=hello
dict['bar']=oil
dict['baz']=world

readonly -a arr
echo arr:${#arr[@]}
readonly -A dict
echo dict:${#dict[@]}
