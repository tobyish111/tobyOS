case $SH in bash|mksh) exit ;; esac

OILS_CRASH_DUMP_DIR=$TMP $SH -ec 'a=({0..3}); unset -v "a[2]"; false'
json read (&crash_dump) < $TMP/*.json
json write (crash_dump.var_stack[0].a)
