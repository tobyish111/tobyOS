# XCU 2.9.4 + 2.14: a redirection applies to a WHOLE compound command, to
# a function CALL, and layers with the function body's own output.
{ echo a; echo b; } > cr97a.txt
cat cr97a.txt
if true; then echo in-if; fi > cr97b.txt
cat cr97b.txt
i=0
while [ $i -lt 2 ]; do echo "w$i"; i=$((i+1)); done > cr97c.txt
cat cr97c.txt
f97() { echo from-fn; }
f97 > cr97d.txt
echo "4=$?"
cat cr97d.txt
( echo sub ) > cr97e.txt
cat cr97e.txt
echo end
