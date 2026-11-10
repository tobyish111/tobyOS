# A builtin's assignment that is refused says so under the builtin's name --
# "printf: `/': not a valid identifier", "((: `0'...", "read: ..." -- an
# assignment or an expansion bare, as bash's builtin_error and report_error
# (finding 331). tsh said all of them bare.
declare -n r
{ printf -v r /; echo "rc=$?"; } 2>&1
{ ((r=0)); echo "rc=$?"; } 2>&1
{ read r <<< '/'; echo "rc=$?"; } 2>&1
{ printf -v r ''; echo "rc=$?"; } 2>&1
declare -p r
