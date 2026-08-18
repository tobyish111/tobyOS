cd /
cd $TMP
echo "old: $OLDPWD"
env | grep OLDPWD  # It's EXPORTED too!
cd -
