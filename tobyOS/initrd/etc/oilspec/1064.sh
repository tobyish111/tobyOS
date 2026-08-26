case $SH in bash|dash|mksh|zsh) exit ;; esac

ulimit -a > short.txt
ulimit --all > long.txt

wc -l short.txt long.txt

diff -u short.txt long.txt
echo status=$?
