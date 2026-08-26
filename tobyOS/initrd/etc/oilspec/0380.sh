case $SH in mksh) exit ;; esac

$SH $REPO_ROOT/spec/testdata/extdebug.sh | sed "s;$REPO_ROOT;ROOT;g"
