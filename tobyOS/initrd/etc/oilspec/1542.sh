set -o errexit
{ test no = yes && echo hi; }
echo status=$?
