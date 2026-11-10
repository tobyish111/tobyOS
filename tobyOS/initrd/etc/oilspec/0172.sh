for name in 'a[' 'a[5'; do
  echo "echo hi from $name: \$# args: \$@" > "$name"
  chmod +x "$name"
done

# syntax error in bash
$SH -c 'a['
echo "a[ status=$?"

$SH -c 'a[5'
echo "a[5 status=$?"

# 1 arg +
$SH -c 'a[5 +'
echo "a[5 + status=$?"

# 2 args
$SH -c 'a[5 + 3]'
echo "a[5 + 3] status=$?"

$SH -c 'a[5 + 3]='
echo "a[5 + 3]= status=$?"

$SH -c 'a[5 + 3]+'
echo "a[5 + 3]+ status=$?"

$SH -c 'a[5 + 3]+='
echo "a[5 + 3]+= status=$?"

# mksh doesn't issue extra parse errors
# and it doesn't turn a[5 + 3] and a[5 + 3]+ into commands!



# in zsh, everything becomes "bad pattern"

a[ status=1
a[5 status=1
a[5 + status=1
a[5 + 3] status=1
a[5 + 3]= status=1
a[5 + 3]+ status=1
a[5 + 3]+= status=1

# ash behavior is consistent
