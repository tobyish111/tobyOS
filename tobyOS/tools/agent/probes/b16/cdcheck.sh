# cdcheck.sh: a logical cd's `..` must back out of a directory (bash's
# PATH_CHECKDOTDOT), or the cd tries the path as written, which the kernel
# walks (Oils 0719). Messages without the shell's own prefix.
exec 2>&1
N() { sed 's/^[^:]*: line [0-9]*: //'; }
cd /tmp; rm -rf cdk; mkdir -p cdk/d/e; cd cdk; touch f; ln -s d l; ln -s nowhere dang
t() { ( cd "$1"; echo "$1 -> $? $PWD" ) 2>&1 | N; }
t nonexistent_ZZ/..
t d/..
t f/..
t l/..
t l/e/../..
t dang/..
t d/../nonexistent/..
t /nonexistent_ZZ/..
t /tmp/cdk/d/../d
t ./d/./e/../..
t ..
( set -o posix; cd nonexistent_ZZ/..; echo "posix $?" ) 2>&1 | N
( cd -P nonexistent_ZZ/..; echo "P $?" ) 2>&1 | N
( cd -L f/..; echo "L $?" ) 2>&1 | N
( cd /; cd ..; echo "root $? $PWD" ) 2>&1 | N
cd /tmp; rm -rf cdk
echo end
