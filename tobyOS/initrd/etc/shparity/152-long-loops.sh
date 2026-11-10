i=0; while ((i++ < 1000001)); do :; done; echo "w $i"
i=0; until ((i++ >= 1000001)); do :; done; echo "u $i"
for ((j=0; j<1000001; j++)); do :; done; echo "f $j"
echo done
