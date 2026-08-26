case $SH in dash|mksh|zsh|ash|yash) exit ;; esac

echo TRUE
type -t true  # builtin
true() { echo true func; }
type -t true  # now a function
echo ---

echo EVAL

type -t eval  # builtin
# define function before set -o posix
eval() { echo "shell function: $1"; }
# bash runs the FUNCTION, but OSH finds the special builtin
# OSH doesn't need set -o posix
eval 'echo before posix'

if test -n "$BASH_VERSION"; then
  # this makes the eval definition invisible!
  set -o posix
fi

eval 'echo after posix'  # this is the builtin eval
# bash claims it's a function, but it's a builtin
type -t eval

# it finds the function and the special builtin
#type -a eval
