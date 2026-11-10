# A subscript that does not evaluate drops the rest of the line in let and
# (( )) as it does in $(( )); a plain arithmetic error is let's own. tsh
# went on with the line (finding 571).
exec 2>&1
i=()
(( i['2']=6 )); echo "A rc=$?"
let "i['4']=8" x=1; echo "C rc=$? x=$x"
let "i[1+]=9"; echo "D rc=$?"
let "j=1+"; echo "E rc=$?"
echo done
