# An array operation through a reference to an element -- mapfile, read -a,
# coproc -- names no array: "mapfile: `XXX[0]': not a valid identifier",
# and nothing is made (finding 339). tsh made an array called "XXX[0]".
{
( declare -n ref=XXX[0]; mapfile ref <<< $'1\n2'; echo "rc=$?"; declare -p XXX[0]; declare -p XXX )
( declare -n r2=YY[1]; read -a r2 <<< "A B"; echo "rc=$?"; declare -p YY )
( declare -n ref=ZZ[0]; coproc ref { :; }; declare -p ${!ref}; wait; declare -p ZZ )
( declare -n e=WW[2]; e=(1 2); echo "rc=$?"; declare -p WW; e=v; declare -p WW )
} 2>&1
