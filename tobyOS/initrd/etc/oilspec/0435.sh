declare -A foo
key=bar
foo["$key"]=value
echo ${foo["bar"]}
