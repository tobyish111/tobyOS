typeset -a a
a=(42)

x='a[$(echo 0 | tee PWNED)]'

echo ${!x}

if test -f PWNED; then
  echo PWNED
  cat PWNED
else
  echo NOPE
fi
