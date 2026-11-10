case $SH in mksh) exit 0 ;; esac

declare -p foo=bar
echo status=$?

a=b
declare -p a foo=bar > tmp.txt
echo status=$?
sed 's/"//g' tmp.txt  # don't care about quotes
