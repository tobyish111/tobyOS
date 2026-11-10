typeset -A A
K=5
V=42
A["$K"]=$V
A["K"]=oops
A[K]=oops2

# We don't neither 42 nor "oops".  Bad!
(( V = A[K] ))

echo V=$V
