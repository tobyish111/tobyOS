set a b c
result=$(echo ${@:0:1})
echo ${result//"$0"/'SHELL'}
