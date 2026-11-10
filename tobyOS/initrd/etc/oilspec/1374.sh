x=${ echo hi;}
echo "[$x]"
echo

# trailing space allowed
x=${ echo one; echo two; }
echo "[$x]"
echo

myfunc() {
  echo ' 3 '
  echo ' 4 5 '
}

x=${ myfunc;}
echo "[$x]"
echo

# SYNTAX ERROR
x=${myfunc;}
echo "[$x]"
