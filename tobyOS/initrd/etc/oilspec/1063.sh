# - bash, zsh, busybox ash accept multiple "commands", which requires custom
#   flag parsing, like

#   ulimit -f 999 -n
#   ulimit -f 999 -n 888
#
# - dash and mksh accept a single ARG
#
# we want to make it clear we're like the latter

# can't print all and -f
ulimit -f -a >/dev/null
echo status=$?

ulimit -f -n >/dev/null
echo status=$?

ulimit -f -n 999 >/dev/null
echo status=$?



# zsh is better - it checks that -a and -f are exclusive
