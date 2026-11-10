case $SH in *bash|*mksh) exit ;; esac

shopt -s strict_nameref

ref=1

echo ref=$ref
typeset -n ref
echo ref=$ref

ref=foo
echo ref=$ref
