# Hm with C.UTF-8, this does no case folding?
export LC_ALL=en_US.UTF-8

x='ABC DEF'
echo ${x,[d-f]}
echo ${x,,[d-f]}  # bash 4.4 fixed in bash 5.2.21
