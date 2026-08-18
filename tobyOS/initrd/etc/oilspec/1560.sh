# bash 5.2 fixed bash 4.4 bug: this is now checked

case $SH in dash) exit ;; esac

$SH -c '
set -o errexit
[[ x = x ]] > /
echo dbracket status=$?
'
echo status=$?

$SH -c '
set -o errexit
(( 42 )) > /
echo dparen status=$?
'
echo status=$?
