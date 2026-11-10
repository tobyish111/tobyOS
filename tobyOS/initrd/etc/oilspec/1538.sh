set -o errexit
test "$mod" = readline && echo "#endif"
echo status=$?
