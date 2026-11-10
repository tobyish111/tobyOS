case $SH in dash | ash) exit 0 ;; esac

myarray=(a 'b c')
IFS=''
argv.py at ${myarray[@]}
argv.py star ${myarray[*]}
