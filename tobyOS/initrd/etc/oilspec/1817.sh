i=0
seq 3 | ( while read foo; do
  i=$((i+1))
  #echo $i
done
echo $i )
