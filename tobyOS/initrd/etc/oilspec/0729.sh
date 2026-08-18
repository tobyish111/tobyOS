dir=/tmp/oil-spec-test/pwd-2
mkdir -p $dir
cd $dir

# ensure clean shell process state
$SH -c 'unset PWD; pwd'
