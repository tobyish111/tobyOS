a0=()
a1=(1)
a2=(1 2)
a=(x y z w)
a[500]=100
a[1000]=100

case $SH in
bash|mksh)
  typeset -p a0 a1 a2 a
  exit ;;
esac

declare -p a0 a1 a2 a
