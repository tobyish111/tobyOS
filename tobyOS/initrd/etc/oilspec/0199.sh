# and $assoc is equivalent to ${assoc[0]}, just like regular arrays
declare -A assoc
assoc[1]=1
assoc[2]=2
echo ${assoc[1]} ${assoc[2]} ${assoc}
assoc[0]=zero
echo ${assoc[1]} ${assoc[2]} ${assoc}
assoc=string
echo ${assoc[1]} ${assoc[2]} ${assoc}
