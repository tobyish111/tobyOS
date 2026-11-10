shopt -s compat_array

declare -A assoc=([ale]=bean [corn]=dip)
ref=assoc
#ref_AT='assoc[@]'

# UNQUOTED doesn't work with the OSH parser
#ref_SUB='assoc[ale]'
ref_SUB='assoc["ale"]'

ref_SUB_QUOTED='assoc["al"e]'

ref_SUB_BAD='assoc["bad"]'

echo ref=${!ref}  # compat_array: assoc is equivalent to assoc[0]
#echo ref_AT=${!ref_AT}
echo ref_SUB=${!ref_SUB}
echo ref_SUB_QUOTED=${!ref_SUB_QUOTED}
echo ref_SUB_BAD=${!ref_SUB_BAD}
