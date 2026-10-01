# The cost of 2000 one-line command substitutions (S15: each one now runs
# inside a saved-and-restored state frame).
s=$EPOCHREALTIME
for i in $(seq 2000); do x=$(echo $i); done
e=$EPOCHREALTIME
echo "x=$x ms=$(( (${e%.*}${e#*.} - ${s%.*}${s#*.}) / 1000 ))"
