cd $REPO_ROOT/spec/testdata/bug-shellopts

#shopt -p no_init_globals

$SH -o ysh:upgrade ./top-level.ysh

#echo ---
#$SH -e -c 'echo SHELLOPTS=$SHELLOPTS'
#$SH -e -o ysh:upgrade -c 'echo SHELLOPTS=$SHELLOPTS'
