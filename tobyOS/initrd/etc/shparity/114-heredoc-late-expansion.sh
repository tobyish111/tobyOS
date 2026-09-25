v=old
if true; then
  v=new
  cat <<EOF
H01 $v
EOF
fi
f2() {
  w=new
  cat <<EOF
H02 $w
EOF
}
w=old; f2
f3() {
  cat <<EOF
H03 arg=$1
EOF
}
f3 one; f3 two
n=0
f4() {
  read r <<EOF
$((n+=1))
EOF
  echo "H04 r=$r"
}
f4; f4; echo "H04 n=$n"
{
  x=inline; cat <<EOF
H07 $x
EOF
}
f8() {
  v=zzz
  cat <<'EOF'
H08 $v $(echo no)
EOF
}
f8
t=top
cat <<EOF
H09 $t
EOF
f10() {
  u=tab
	cat <<-EOF
	H10 $u
	EOF
}
f10
f11() {
  y=val
  cat <<EOF
H11 \$y $y \\ end
EOF
}
f11
f12() {
  p=P; q=Q
  cat <<A; cat <<B
H12a $p
A
H12b $q
B
}
f12
f13() {
  z=sub
  cat <<EOF
H13 $(echo $z)
EOF
}
f13
r=outer
r=prefix cat <<EOF
H14 $r
EOF
f15() {
  s=line
  read got <<EOF
$s-read
EOF
  echo "H15 $got"
}
f15
(
  m=sub
  cat <<EOF
H16 $m
EOF
)
case x in
  x) c=arm
     cat <<EOF
H17 $c
EOF
     ;;
esac
echo done
