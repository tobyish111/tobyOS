# NAMEREFS -- a variable whose value is the NAME of another.
#
# `typeset -n ref=x; echo $ref` prints x's value, not the string "x", and
# an assignment to the reference writes through it. Two of them can point
# at each other, which is why every read follows the chain with a hop
# limit rather than recursing.
x=XX
typeset -n ref=x
echo A=$ref
ref=YY
echo B=$x C=$ref
typeset -n r2=ref
echo D=$r2
typeset -n ref1=ref2
typeset -n ref2=ref1
echo E=defined
echo F=[$ref1]
echo G=[$ref2]
typeset -n missing=nosuchvar
echo H=[$missing]
echo done
