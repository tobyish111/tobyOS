if [[ 'a  b' =~ (a  b) ]]; then
  echo one
fi

if [[ 'a b' =~ (a  b) ]]; then
  echo BAD
fi

if [[ 'a b' =~ (a b|c) ]]; then
  echo two
fi

# I think spaces are only allowed within ()

if [[ '  c' =~ (a|  c) ]]; then
  echo three
fi
