case $SH in dash|mksh) exit ;; esac

shopt -s ignore_shopt_not_impl

opt_name=xpg_echo

shopt -p xpg_echo
shopt -q xpg_echo; echo q=$?

shopt -s xpg_echo
shopt -p xpg_echo

shopt -u xpg_echo
shopt -p xpg_echo
echo p=$?  # weird, bash also returns a status

shopt xpg_echo >/dev/null
echo noflag=$?

shopt -o errexit >/dev/null
echo set=$?
