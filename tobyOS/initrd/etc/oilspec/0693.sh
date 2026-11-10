# not isolated: /tmp usually has sticky bit on
# https://en.wikipedia.org/wiki/Sticky_bit

test -k /tmp
echo status=$?

test -k /bin
echo status=$?
