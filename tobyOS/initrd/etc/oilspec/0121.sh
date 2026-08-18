# zsh does 1-based indexing!
array=(1 2 3 4)
echo $((array[1] + array[2]*3))
