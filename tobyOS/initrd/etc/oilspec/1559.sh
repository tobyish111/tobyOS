# bash 5.2 fixed bash 4.4 bug: this is now checked

$SH -c '
shopt -s expand_aliases

set -o errexit
alias zz="{ echo 1; echo 2; }"
zz > /
echo alias status=$?
'
echo status=$?
