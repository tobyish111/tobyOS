dir=$TMP/symlinktest
mkdir -p $dir
cd $dir
mkdir -p a/b/c
mkdir -p a/b/d
ln -s -f a/b/c c > /dev/null
cd c
cd ..
# Expecting a c/ (since we are in symlinktest) but osh gives c d (thinks we are
# in b/)
ls
