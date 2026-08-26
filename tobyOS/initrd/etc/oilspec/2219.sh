case $SH in dash|mksh) exit ;; esac

shopt -po nounset
set -o nounset
shopt -po nounset

echo --

shopt -po | egrep -o 'errexit|noglob|nounset'
