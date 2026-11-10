set -- a 'b c'
IFS=''
printf '[%s]\n' $@
printf '[%s]\n' $*
