# bash declare -p will print binary data, which makes this invalid UTF-8!
foo=$(/bin/echo -e 'a\nb\xffc'\'d)

# let's test the easier \x01, which doesn't give bash problems
foo=$(/bin/echo -e 'a\nb\x01c'\'d)

# dash:
#   only supports 'set'; prints it on multiple lines with binary data
#   switches to "'" for single quotes, not \'
# zsh:
#   print binary data all the time, except for printf %q
#   does print $'' strings
# mksh:
#   prints binary data for @Q
#   prints $'' strings

# All are very inconsistent.

case $SH in dash|mksh|zsh) return ;; esac


set | grep -A1 foo

# Will print multi-line and binary data literally!
#declare -p foo

printf 'pf  %q\n' "$foo"

echo '@Q ' ${foo@Q}
