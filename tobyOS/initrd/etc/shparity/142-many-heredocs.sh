read -r a <<A; read -r b <<B; read -r c <<C; read -r d <<D; read -r e <<E; read -r f <<F
one
A
two
B
three
C
four
D
five
E
six
F
echo "h1 [$a $b $c $d $e $f]"
cat <<A <<B <<C <<D <<E <<F | sed 's/^/h2 /'
a
A
b
B
c
C
d
D
e
E
f
F
g() {
  read -r x <<X; read -r y <<Y; read -r z <<Z; read -r w <<W; read -r v <<V
x1
X
y1
Y
z1
Z
w1
W
v1
V
  echo "h3 [$x $y $z $w $v]"
}
g; g
echo done
