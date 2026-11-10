case $SH in bash) exit ;; esac

touch -- foo.txt -foo.txt

shopt -s no_dash_glob  # YSH option

echo *  # expansion does NOT include -foo.txt

GLOBIGNORE=f*.txt
echo *  # expansion includes -foo.txt, because it doesn't match GLOBIGNORE
