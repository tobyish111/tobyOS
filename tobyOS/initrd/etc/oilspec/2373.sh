case $SH in zsh) exit ;; esac

IFS=x
chicken() { for i in "$@"; do echo =$i=; done;}
chicken one abc dxf ghi

echo ---
myfunc() { "$SH" -c 'IFS=x; for i in $@; do echo =$i=; done' blah "$@"; }
myfunc one "" two
