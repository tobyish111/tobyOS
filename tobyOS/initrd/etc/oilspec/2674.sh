# bash is inconsistent because it does it for pipelines and assignments, but
# not (( and [[

case $SH in dash|mksh) exit ;; esac

echo simple
(( a = 2 + 3 ))
echo "(( $_"

[[ a == *.py ]]
echo "[[ $_"
