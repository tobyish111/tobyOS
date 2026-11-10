tmp=$TMP/builtins-pwd-2
mkdir -p $tmp
mkdir -p $tmp/target
ln -s -f $tmp/target $tmp/symlink

cd $tmp/symlink
$SH -c 'basename $(pwd)'
unset PWD
$SH -c 'basename $(pwd)'
