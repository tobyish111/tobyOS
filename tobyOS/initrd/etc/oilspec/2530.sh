set -- '1 2' '3 4'
argv.py "X${unset=x"$@"x}X"
argv.py "X${unset=x$@x}X"  # OSH is the same here

# Bash 4.2..4.4 had a bug. This was fixed in Bash 5.0.
#
# ## BUG bash STDOUT:
# ['Xx1', '2', '3', '4xX']
# ['Xx1 2 3 4xX']
# ## END
