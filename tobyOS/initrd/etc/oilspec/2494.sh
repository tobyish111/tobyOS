array=(1 2 3)

argv.py ${array[@]}

# *** implicit length of N **
argv.py ${array[@]:0}

# Why is this one not allowed
#argv.py ${array[@]:}

# ** implicit length of ZERO **
#argv.py ${array[@]::}
#argv.py ${array[@]:0:}

argv.py ${array[@]:0:0}
echo

# Same agreed upon permutations
set -- 1 2 3
argv.py ${@}
argv.py ${@:1}
argv.py ${@:1:0}
echo

s='123'
argv.py "${s}"
argv.py "${s:0}"
argv.py "${s:0:0}"
