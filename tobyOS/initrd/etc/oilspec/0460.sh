case $SH in bash-4.4|dash|mksh|zsh) exit 99 ;; esac

# We pretend that the variable does not exist when the variable is not
# representable with the "declare -p" format.

var d = {}
declare -p d
