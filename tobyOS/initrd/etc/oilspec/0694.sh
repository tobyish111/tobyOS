tmp=$TMP/builtin-test-1
mkdir -p $tmp
touch $tmp/zz
ln -s -f $tmp/zz $tmp/symlink
ln -s -f $tmp/__nonexistent_ZZ__ $tmp/dangling
test -L $tmp/zz || echo no
test -h $tmp/zz || echo no
test -f $tmp/symlink && echo is-file
test -L $tmp/symlink && echo symlink
test -h $tmp/symlink && echo symlink
test -L $tmp/dangling && echo dangling
test -h $tmp/dangling  && echo dangling
test -f $tmp/dangling  || echo 'dangling is not file'
