set -- 4 5 6

result=$(argv.py ${@:0})
echo ${result//"$0"/'SHELL'}

argv.py ${@:1}
argv.py ${@:2}
