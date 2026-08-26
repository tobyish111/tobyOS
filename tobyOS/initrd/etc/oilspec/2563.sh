# bash allows this construct, but the indirection fails when the array has more
# than one element because the variable name contains a space.  OSH originally
# made it an error unconditionally because [@] implies it's an array, so the
# behavior has been different from Bash when the array has a single element.
# We now changed it to follow Bash even when the array has a single element.

(argv.py "${!a[@]-default}")
echo status=$?

a=(x y z)
(argv.py "${!a[@]-default}")
echo status=$?

# Bash 4.4 had been generating an empty string for ${!undef[@]-}, but this was
# fixed in Bash 5.0.
#
# ## BUG bash status: 0
# ## BUG bash STDOUT:
# ['default']
# status=0
# status=1
# ## END
