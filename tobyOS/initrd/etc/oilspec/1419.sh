# shells differ in whether they actually execve('one/cmd') and get EPERM

mkdir -p one two
PATH="one:two:$PATH"

rm -f one/mycmd two/mycmd
echo 'echo one' > one/mycmd
echo 'echo two' > two/mycmd

# only make the second one executable
chmod +x two/mycmd
mycmd
echo status=$?
