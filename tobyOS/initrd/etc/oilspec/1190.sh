debuglog() {
  echo "  [$@]"
}
trap 'debuglog $LINENO' DEBUG

for x in 1 2; do
  echo x=$x
done

echo ok


# NOT matching bash right now because 'while' loops don't have it
# And we have MORE LOOPS
#
# What we really need is a trap that runs in the main loop and TELLS you what
# kind of node it is?
