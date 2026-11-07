# An expansion error inside eval's string, or a trap's action, throws away
# the rest of ITS line only: the string's next line runs, as a script's
# would, and eval's status is its last command's. tsh threw the rest of
# the whole string away (finding 627).
exec 2>&1
eval 'x=$((1+)); echo "not this"
echo "the next line of the string"'; echo "eval: $?"
eval 'echo one
echo ${a[}; echo "not this"
echo three'; echo "eval: $?"
eval 'echo first
y=$((2/0))'; echo "a last line's error: $?"
trap 'z=$((3+)); echo "not this"
echo "the trap went on"' USR1
kill -USR1 $$; echo "after the trap: $?"
