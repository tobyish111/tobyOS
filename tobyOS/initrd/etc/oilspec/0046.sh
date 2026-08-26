# dash enters an infinite loop!
case $SH in
  dash)
    exit 1
    ;;
esac

set -x
PS4='+$(echo trace) '
shopt -s expand_aliases
alias a=argv.py
a foo bar
