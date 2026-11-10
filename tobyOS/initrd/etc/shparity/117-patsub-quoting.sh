v=$'a\nb\nc'
echo "P01 [${v//$'\n'/|}]"
echo "P02 [${v/$'\n'/|}]"
echo "P03 [${v#*$'\n'}]"
echo "P04 [${v##*$'\n'}]"
echo "P05 [${v%$'\n'*}]"
echo "P06 [${v%%$'\n'*}]"
t="x|y"; echo "P07 [${t//|/$'\t'}]" | od -c | head -1
u=; echo "P08 [${u:-$'d\te'}]" | od -c | head -1
w="a*b"; echo "P09 [${w//$'*'/S}]"
w="a*b"; echo "P10 [${w//*/S}]"
[[ $v == *$'\n'* ]] && echo P11 yes || echo P11 no
x="it's"; echo "P12 [${x//$'\''/_}]"
y="tab\there"; echo "P13 [${y//$'\t'/_}]"
z=$(printf "a	b"); echo "P14 [${z//$'\t'/_}]"
echo "P15 [${v//$'\n'}]"
echo P16 "${v//$'\n'/ }"
x="it's"; echo "P17 [${x#*$'\''}]"
sep=+; echo "P19 [${v//$'\n'/${sep}}]"
echo P20 ${v//$'\n'/ }
x=a/b/c; echo "P21 [${x//\//_}] [${x//'/'/_}] [${x//\"/\"/_}] [${x/\//_}]"
x=; echo "P22 [${x//*/S}] [${x///S}] [${x/#/S}] [${x/%/S}]"
x=abc; echo "P23 [${x//*/S}] [${x//b*/S}] [${x//*b/S}] [${x/%/S}] [${x/#/S}] [${x///S}]"
p='*'; x='a*b'; echo "P24 [${x//$p/S}] [${x//\"$p\"/S}] [${x//\*/S}]"
arr=($'a\nb' c); echo "P25 [${arr[@]//$'\n'/|}]"
echo done
