x=${|y=" reply var "; REPLY=$y}
echo "[$x]"
echo

echo '  from file  ' > tmp.txt

x=${|read -r < tmp.txt}
echo "[$x]"
echo

# SYNTAX ERROR
x=${ |REPLY=zz}
echo "[$x]"
