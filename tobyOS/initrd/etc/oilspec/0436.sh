# bug caught by my feature detection snippet in bash-completion
if ! foo=bar; then
  echo BAD
fi
echo $foo
if ! eval 'spam=eggs'; then
  echo BAD
fi
echo $spam
