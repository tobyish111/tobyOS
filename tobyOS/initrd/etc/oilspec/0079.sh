# The first paren is part of the math, parens 2 and 3 are a single token ending
# arith sub.
((a=1 + (2*3)))
echo $a $((1 + (2*3)))
