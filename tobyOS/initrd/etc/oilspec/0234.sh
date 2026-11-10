case $SH in mksh) exit ;; esac

z=()
declare -a | grep z=

z+=(b c)
declare -a | grep z=

# z[5]= finds the index, or puts it in SORTED order I think
z[5]=d
declare -a | grep z=

z[1]=ZZZ
declare -a | grep z=

# Adds after last index
z+=(f g)
declare -a | grep z=

# This is the equivalent of z[0]+=mystr
z+=-mystr
declare -a | grep z=

z[1]+=-append
declare -a | grep z=

argv.py keys "${!z[@]}"  # 0 1 5 6 7
argv.py values "${z[@]}"

# can't do this conversion
declare -A z
declare -A | grep z=

echo status=$?
