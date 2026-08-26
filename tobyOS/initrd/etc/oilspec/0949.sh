case $SH in dash|zsh|mksh|ash) exit ;; esac

$SH -c 'declare a=(x y); declare -p a'
if test $? -ne 0; then
  echo 'fail'
fi

$SH -c 'builtin declare a=(x y); declare -p a'
if test $? -ne 0; then
  echo 'fail'
fi

$SH -c 'builtin declare -a a=(x y); declare -p a'
if test $? -ne 0; then
  echo 'fail'
fi
