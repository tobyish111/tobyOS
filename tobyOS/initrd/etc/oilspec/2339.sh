case $SH in bash*|dash|mksh) exit ;; esac

$SH -c '
a=(1 2 3); echo /${a[@]::}/
'
echo status=$?

$SH -c '
shopt --set strict_parse_slice

a=(1 2 3); echo /${a[@]::}/
'
echo status=$?
