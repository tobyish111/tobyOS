case $SH in dash) exit ;; esac

$SH -c '
shopt -s lastpipe
set -o errexit
set -o pipefail

ls | false | wc -l'
echo status=$?

# Why does this give status 0?  It should fail

$SH -c '
shopt -s lastpipe
shopt -s no_fork_last  # OSH only
set -o errexit
set -o pipefail

ls | false | wc -l'
echo status=$?
