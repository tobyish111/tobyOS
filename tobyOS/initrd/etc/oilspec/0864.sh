mkdir -p dir
echo "echo path" > dir/cmd
. dir/cmd
rm dir/cmd
