# mksh, dash do not implement {fd} redirections.
case $SH in mksh|dash) exit 1 ;; esac
# oil 0.8.pre4 fails to close fd by {fd}&-.
exec {fd}>file1
echo foo55 >&$fd
exec {fd}>&-
echo bar >&$fd
cat file1
