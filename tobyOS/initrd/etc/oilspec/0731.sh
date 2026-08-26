dir=/tmp/oil-spec-test/pwd
mkdir -p $dir
cd $dir
pwd
rmdir $dir
echo status=$?
pwd
echo status=$?
