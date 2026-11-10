case $SH in dash | mksh | ash | yash) exit 0 ;; esac

IFS=''
a=(v1 v2 v3)
argv.py at ${!a[@]}
argv.py star ${!a[*]}
