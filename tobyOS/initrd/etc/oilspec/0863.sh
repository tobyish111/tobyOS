cd $TMP
mkdir -p dir
echo "echo path" > dir/cmd
echo "echo current dir" > cmd
PATH="dir:$PATH"
. cmd
echo status=$?
