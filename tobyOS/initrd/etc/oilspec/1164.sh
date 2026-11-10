# Bash only implements it behind the posix option
case $SH in
  bash) set -o posix ;;
esac
foo=bar readonly spam=eggs
echo foo=$foo
echo spam=$spam

# should NOT be exported
printenv.py foo
printenv.py spam
