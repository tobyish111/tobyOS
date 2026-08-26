func2=x  # var names are NOT found
declare -f myfunc func2
echo $?

myfunc() { echo myfunc; }
declare -f myfunc func2 > /dev/null
echo $?

func2() { echo func2; }
declare -f myfunc func2 > /dev/null
echo $?
