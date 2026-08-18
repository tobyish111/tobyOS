shopt -s lastpipe
i=0
seq 3 | while read line; do
  (( i++ ))
done
echo i=$i
