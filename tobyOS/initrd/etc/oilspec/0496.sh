case ${SH##*/} in
(dash|ash|yash|mksh) exit 1;; # dash/ash/yash/mksh does not have associative arrays
(osh) shopt -s compat_array;;
(zsh) setopt KSH_ARRAYS;;
esac

declare -A d=()
d['0']=1
d['foo']=hello
d['bar']=world
((d++))
argv.py ${d['0']} ${d['foo']} ${d['bar']}
