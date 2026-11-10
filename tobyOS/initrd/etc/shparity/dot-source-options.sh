# . and source take no options: `. -i FILE` is ".: -i: invalid option" and
# the usage line, status 2, where tsh looked for a file -i; `--` still ends
# the options (finding 418).
exec 2>&1
. -i /dev/null; echo "rc=$?"; source -x /dev/null; echo "rc=$?"; . -- /dev/null; echo "rc=$?"
