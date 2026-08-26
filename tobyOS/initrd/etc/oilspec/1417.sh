cd $TMP
PATH="one:two:$PATH"
mkdir -p one two
rm -f one/* two/*
echo 'echo two' > two/mycmd
chmod +x two/mycmd
mycmd

# Insert earlier in the path
echo 'echo one' > one/mycmd
chmod +x one/mycmd
mycmd  # still runs the cached 'two'

# clear the cache
hash -r
mycmd  # now it runs the new 'one'


# zsh doesn't do caching!
