case $SH in bash) ;; *) shopt --set strict_array ;; esac

declare -A a=([a]=b)
eval "a=(1 2 3 4)"
declare -p a
