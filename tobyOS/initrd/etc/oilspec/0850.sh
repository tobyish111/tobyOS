cd $TMP
echo 'exit 42' > lib.sh
. ./lib.sh
echo 'should not get here'
