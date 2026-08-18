declare -a array
array[x=1]='one'

code='y=2'
#code='1+2'  # doesn't work either
array[$code]='two'

argv.py "${array[@]}"
echo x=$x
echo y=$y
