case $SH in mksh) exit ;; esac

declare -A A=([k]=v)
declare -A | grep A=

argv.py keys "${!A[@]}"
argv.py values "${A[@]}"

exit

# Huh this actually works, we don't support it
# Hm the order here is all messed up, in bash 5.2
A+=([k2]=v2 [0]=foo [9]=9 [9999]=9999)
declare -A | grep A=

A+=-append
declare -A | grep A=

argv.py keys "${!A[@]}"
argv.py values "${A[@]}"
