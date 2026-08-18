case $SH in dash) exit ;; esac

mu1='[μ]'
mu2=$'[\u03bc]'

set -o xtrace
echo "$mu1" "$mu2"
