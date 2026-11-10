case ${SH##*/} in
(dash|ash) exit 1;; # dash/ash does not have arrays
(osh) shopt -s compat_array;;
(zsh) setopt KSH_ARRAYS;;
esac

a=(1 0 0)
: $(( a++ ))
argv.py "${a[@]}"
