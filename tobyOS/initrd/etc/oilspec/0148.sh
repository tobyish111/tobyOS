a=(4 5 6)

# zsh and osh can't evaluate the array like that
# which is consistent with their behavior on $(( a ))

echo $(( a, last = a[2], 42 ))
echo last=$last
