echo -n '' > $TMP/empty.txt
read x < $TMP/empty.txt
argv.py "status=$?" "$x"

# No variable name, behaves the same
read < $TMP/empty.txt
argv.py "status=$?" "$REPLY"
