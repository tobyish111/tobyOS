# A blank inside the subscript an array literal's element starts with is
# part of it -- `([x y]=z)` is the key "x y", `([1 + 1]=x)` index 2 -- where
# tsh broke the element at the blank (finding 352).
{
declare -A wheat=([six]=6); wheat+=([x y]=z [p q r]=s); declare -p wheat
a=([1 + 1]=x [3]=y\[z\]); declare -p a; b=( [k]=v w[1] ); declare -p b
} 2>&1
