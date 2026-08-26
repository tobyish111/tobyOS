shopt -s lastpipe  # for bash

seq 2 | mapfile m
seq 3 | readarray r
echo ${#m[@]}
echo ${#r[@]}
