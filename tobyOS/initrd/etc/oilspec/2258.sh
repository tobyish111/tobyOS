case $SH in bash|dash|mksh|zsh) exit ;; esac

echo 'echo one "$@"' > one.sh
echo 'echo fail "$@"; ( exit 42 )' > fail.sh

$SH --eval one.sh \
  -c 'echo status=$? flag -c "$@"' dummy x y z
echo

# Even though errexit is off, the shell exits if the last status of an --eval
# file was non-zero.

$SH --eval one.sh --eval fail.sh \
  -c 'echo status=$? flag -c "$@"' dummy x y z
echo status=$?
