for i in a b; do
  cat <<EOF
H05 i=$i
EOF
done
k=0
while [ $k -lt 2 ]; do
  k=$((k+1))
  cat <<EOF
H06 k=$k
EOF
done
for a in 1 2; do
  for b in x y; do
    cat <<EOF
H18 $a$b
EOF
  done
done
for c in 1 2 3; do
  cat <<EOF
H19 pre $c
EOF
  [ $c = 2 ] && continue
  cat <<EOF
H19 post $c
EOF
done
u=0
until [ $u -ge 2 ]; do
  u=$((u+1))
  cat <<EOF
H20 u=$u
EOF
done
for ((j=0; j<2; j++)); do
  cat <<EOF
H21 j=$j
EOF
done
f22() {
  for i in 1 2; do
    cat <<EOF
H22 in $i
EOF
  done
  cat <<EOF
H22 after
EOF
}
f22
echo done
