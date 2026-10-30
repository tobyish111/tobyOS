# declare -n's += joins the reference's own value, and a join that names the
# reference is refused, "NAME: nameref variable self references not
# allowed", status 1, the value kept (finding 359). tsh made it name itself.
exec 2>&1
typeset -n ref=re ref+=f; echo "rc=$?"; typeset -p ref
ref=4; typeset -p ref re
declare -n r2=ab; declare -n r2+=c; echo "rc=$?"; declare -p r2
