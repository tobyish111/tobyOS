# BRACE EXPANSION -- the first expansion, and purely textual.
#
# It happens before parameter expansion, which is why `x={a,b}; echo $x`
# prints the braces back: they arrived from a variable, after the brace pass
# had already gone by. Doing it on the source text before the tokenizer
# reads it is what makes that ordering fall out for free.
echo {a,b,c}
echo pre{a,b}post
echo {1..5}
echo {5..1}
echo {a..e}
echo {a,b}{c,d}
echo {1..6..2}
echo x{a}y
echo x{}y
echo "{a,b}"
x={a,b}
echo $x
echo a,b
echo {a,b}.txt
{ echo group, with comma; }
echo done
