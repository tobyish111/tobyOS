p() { printf '[%s]' "$1"; printf '\n'; }
if true; then
  v='a
b'
fi
p "Q01 $v"
for i in 1; do
  v="c
d"
done
p "Q02 $v"
f3() {
  v='e
f'
}
f3; p "Q03 $v"
while true; do
  v='g
h'
  break
done
p "Q04 $v"
case x in
  x) v='k
l' ;;
esac
p "Q05 $v"
(
  v='m
n'
  p "Q06 $v"
)
{
  v="o
p"
}
p "Q07 $v"
until false; do
  v='q
r'
  break
done
p "Q08 $v"
if true; then
  v='one

   three'
fi
p "Q09 $v"
if true; then
  v='x
fi
done
;;
)
y'
fi
p "Q10 $v"
if true; then
  v='s\
t'
fi
p "Q11 $v"
if true; then
  v="u\
w"
fi
p "Q12 $v"
if true; then
  v=$'x
y\tz'
fi
p "Q13 $v"
for i in 1; do
  if true; then
    v='nest
ed'
  fi
done
p "Q14 $v"
f15() {
  cat <<EOF
don't stop
it's fine
EOF
  echo after-heredoc
}
f15
if true; then
  # it's a comment with an apostrophe
  v=single
fi
p "Q16 $v"
if true; then v='one line'; w="two"; fi
p "Q17 $v $w"
if true; then
  v=$(echo 'p
q')
fi
p "Q18 $v"
if true; then
  echo "start
end"; fi
if true; then
  a=('x
y' z)
fi
p "Q21 ${a[0]}|${a[1]}|${#a[@]}"
g22() {
  local m='usage:
  -h  help
  -v  verbose'
  printf '%s\n' "$m"
}
g22
if true; then
  v='a
b'##x
fi
p "Q23 $v"
if true; then
  v='c
d'   # a comment with it's quote
fi
p "Q24 $v"
if true; then
  v='e
# not a comment
  f' w="g
  h"
fi
p "Q25 $v|$w"
if true; then
  v='i  
j'
  cat <<EOF
Q26 it's here
EOF
fi
echo done
