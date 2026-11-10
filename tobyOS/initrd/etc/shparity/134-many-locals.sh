f() {
  local i=0
  while [ $i -lt 600 ]; do eval "local l_$i=$i"; i=$((i+1)); done
  echo "o1 [$l_0] [$l_255] [$l_256] [$l_599]"
  g
}
g() { echo "o2 [$l_300]"; }
f
echo "o3 [${l_300-gone}]"
# ...and one local per frame of a 300-deep recursion (nesting was capped at 8).
h() { local v=$1; [ $v -gt 0 ] && h $((v - 1)); r=$((r + v)); }; r=0; h 300; echo "o4 $r [${v-unset}]"
echo done
