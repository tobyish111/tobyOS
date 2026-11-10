case $SH in dash|zsh) exit ;; esac

false
echo pipestatus ${PIPESTATUS[@]}

exit 55 | (exit 44)
echo pipestatus ${PIPESTATUS[@]}

true
echo pipestatus ${PIPESTATUS[@]}
