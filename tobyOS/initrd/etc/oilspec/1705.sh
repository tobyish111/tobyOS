case $SH in dash|ash|mksh) exit ;; esac

shopt -u globskipdots
echo hi .*
