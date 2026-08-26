cd $TMP
mkdir -p one two
echo 'echo one' > one/mycmd
echo 'echo two' > two/mycmd
chmod +x one/mycmd two/mycmd

PATH='one:two'
mycmd
