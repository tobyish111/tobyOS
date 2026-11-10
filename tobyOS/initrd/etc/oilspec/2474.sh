path=~/git/oilshell

# ~ expansion occurs
#echo path=$path

echo ${path//~/z}

echo ${path/~/z}
