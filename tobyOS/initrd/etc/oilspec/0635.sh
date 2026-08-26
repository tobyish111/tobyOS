rm -f PWNED

x='a[<(echo 42 | tee PWNED)]=1'
echo $(( x ))

if test -f PWNED; then
  cat PWNED
else
  echo NOPE
fi



# bash keeps going
