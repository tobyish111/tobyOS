# but ${!A[@]/b/B} doesn't work
declare -A A
A['aa']=bbb
A['bb']=ccc
A['cc']=ddd
for val in "${A[@]//b/B}"; do
  echo $val
done | sort
