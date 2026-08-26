IFS=x
A=xabcxx
for i in $A; do echo =$i=; done
echo

unset IFS
A="   abc   def   "
for i in ""$A""; do echo =$i=; done
