x=XX
y=YY

ref=x
ref=y
echo 1 ref=$ref

# now it's a reference
typeset -n ref

echo 2 ref=$ref  # prints YY

ref=XXXX
echo 3 ref=$ref  # it actually prints y, which is XXXX

# now Y is mutated!
echo 4 y=$y
