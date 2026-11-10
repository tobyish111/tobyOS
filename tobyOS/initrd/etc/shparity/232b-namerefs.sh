# Namerefs: ${!ref}, element targets, the checks declare -n and local -n
# make -- a self reference is an error at top level and a warning in a
# function, an empty value still makes the local.
# (batch 12; probe p28 in C:\t\b12)
exec 2>&1
declare -n x=x; echo "1 s=$?"; declare -p x 2>&1
declare -n y; y=y; echo "2 s=$?"; declare -p y
declare -n z='a[1]'; echo "3 s=$? $(declare -p z)"
declare -n w='a[]'; echo "4 s=$?"; declare -p w
declare -n v='1a'; echo "5 s=$?"
local_t() { local -n q='#'; echo "6 s=$?"; }; local_t
declare -n u=; echo "7 s=$?"; declare -p u
declare -n t='a b'; echo "8 s=$?"
p=@; declare -n p; echo "9 s=$?"; declare -p p
