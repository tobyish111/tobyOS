cd $TMP
echo "echo current dir" > cmd
. cmd
echo status=$?

# This is a special builtin so failure is fatal.
