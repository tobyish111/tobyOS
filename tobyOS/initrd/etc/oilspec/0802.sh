message=$(popd 2>&1)
echo $?
echo "$message" | grep -o "directory stack"
