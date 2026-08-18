x=foo
typeset -n ref=x
typeset -n ref_to_ref=ref
echo ref_to_ref=$ref_to_ref
echo ref=$ref
