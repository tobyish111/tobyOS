# The whole point of "${a[@]}": one word per element, even inside quotes,
# with embedded spaces preserved. "${a[*]}" is the opposite -- a single
# word joined by the first character of IFS.
a=("one two" three "four  five")
for x in "${a[@]}"; do echo "at:[$x]"; done
for x in "${a[*]}"; do echo "star:[$x]"; done
for x in ${a[@]}; do echo "raw:[$x]"; done
set -- "${a[@]}"
echo "argc=$# 1=[$1] 2=[$2] 3=[$3]"
IFS=:
echo "ifs-star=[${a[*]}]"
IFS=' 	
'
echo end
