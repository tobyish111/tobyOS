# mksh started being flaky on the continuous build and during release.  We
# don't care!  Related to issue #330.
case $SH in mksh) exit ;; esac

: 3>&3
echo hello
