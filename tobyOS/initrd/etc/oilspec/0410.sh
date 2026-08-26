readonly r=r1
echo r=$r

# clear the readonly flag.  Why is this accepted in bash, but doesn't do
# anything?
typeset +r r=r2 
echo r=$r

r=r3
echo r=$r


# mksh doesn't allow you to unset

# bash doesn't allow you to unset
