case $SH in dash|ash|mksh) exit ;; esac

set -o errexit
shopt -s inherit_errexit || true
#shopt -s strict_errexit || true
shopt -s command_sub_errexit || true

# We don't want silent failure here
readonly -a myarray=( one "$(date %x)" two )

#echo len=${#myarray[@]}
argv.py "${myarray[@]}"
