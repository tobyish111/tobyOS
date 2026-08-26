case ${SH##*/} in dash|ash) exit 1 ;; esac # dash/ash does not have arrays
case ${SH##*/} in osh) shopt -s compat_array ;; esac
case ${SH##*/} in zsh) setopt KSH_ARRAYS ;; esac
arr=(foo bar baz)
argv.py "$arr" "${arr}"
