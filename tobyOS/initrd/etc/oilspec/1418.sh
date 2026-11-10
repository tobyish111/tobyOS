cd $TMP
PATH="one:two:$PATH"
mkdir -p one two
rm -f one/mycmd two/mycmd

echo 'echo two' > two/mycmd
chmod +x two/mycmd
mycmd
echo status=$?

# Insert earlier in the path
echo 'echo one' > one/mycmd
chmod +x one/mycmd
rm two/mycmd
mycmd  # still runs the cached 'two'
echo status=$?


# mksh and zsh correctly searches for the executable again!
