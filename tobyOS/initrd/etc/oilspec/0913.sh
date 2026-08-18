cd $TMP

# Populate a history file with a command to be overwritten
echo 'cmd old' > tmp
HISTFILE=tmp
history -c
echo 'cmd new' > /dev/null
history -w # Overwrite history file

# Verify that old command is gone
grep 'old' tmp > /dev/null
echo "found=$?"
