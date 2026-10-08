# A LOGICAL `..` BACKS OUT OF A DIRECTORY, or the cd fails: bash's
# canonicalization checks what each `..` leaves (PATH_CHECKDOTDOT), and when
# that fails tries the path as written, which the kernel walks as Linux does.
# `cd nonexistent_ZZ/..` is "No such file or directory", 1 (Oils 0719, POSIX);
# tsh took the text alone and went to the directory it named. A link to a
# directory is one; a file is "Not a directory" (logs/b16 cdcheck).
exec 2>&1
N() { sed 's/^[^:]*: line [0-9]*: //'; }
d=${TMP:-/tmp}/fx298.$$; rm -rf "$d"; mkdir -p "$d/d/e"; cd "$d" || exit 1
touch f; ln -s d l; ln -s nowhere dang
t() { ( cd "$1"; echo "$1 -> $? ${PWD#$d}" ) 2>&1 | N; }
t nonexistent_ZZ/..
t d/..
t f/..
t l/..
t l/e/../..
t dang/..
t d/../nonexistent/..
t /nonexistent_ZZ/..
t ./d/./e/../..
( set -o posix; cd nonexistent_ZZ/..; echo "posix $?" ) 2>&1 | N
( cd -P nonexistent_ZZ/..; echo "P $?" ) 2>&1 | N
( cd -L f/..; echo "L $?" ) 2>&1 | N
cd /; rm -rf "$d"
echo end
