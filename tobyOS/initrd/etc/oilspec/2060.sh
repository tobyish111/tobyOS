case $SH in dash|ash) exit ;; esac

echo hello > OSCFLAGS

[[ `cat OSCFLAGS` = hello ]] > OSCFLAGS
echo status=$?

# it is the empty string!
[[ `cat OSCFLAGS` = '' ]] > OSCFLAGS
echo status=$?
