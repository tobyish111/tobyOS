tmp=$TMP/builtins-pwd-1
mkdir -p $tmp/target
ln -s -f $tmp/target $tmp/symlink

cd $tmp/symlink

echo pwd:
basename $(pwd)

echo pwd -P:
basename $(pwd -P)
