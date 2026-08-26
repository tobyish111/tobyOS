foo=foo_value
dir=$TMP/'$foo'  # Directory name with a dollar!
mkdir -p $dir
cd $dir
PS1='\w $foo'
test "${PS1@P}" = "$PWD foo_value"
echo status=$?
