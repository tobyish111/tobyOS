x=X
typeset -n ref #=x
echo hi

# bash prints warning: REMOVES the nameref attribute here!
ref=(x y)
echo ref=$ref
