# if/elif/else over the `[` builtin. Note `[` is a COMMAND, not syntax, so a
# shell that special-cases it as syntax will diverge the moment a variable
# expands to something with a space in it.
if [ 1 -eq 1 ]; then echo eq; fi
if [ 1 -ne 1 ]; then echo bad; else echo ne-else; fi
if [ -z "" ]; then echo empty; fi
if [ -n "x" ]; then echo nonempty; fi
if [ "a" = "a" ]; then echo streq; fi
if [ "a" != "b" ]; then echo strneq; fi
if [ 5 -gt 3 ] && [ 2 -lt 4 ]; then echo both; fi
if [ 5 -lt 3 ] || [ 2 -lt 4 ]; then echo either; fi
v=2
if [ "$v" -eq 1 ]; then
    echo one
elif [ "$v" -eq 2 ]; then
    echo two
else
    echo other
fi
s="two words"
if [ "$s" = "two words" ]; then echo spaced-compare; fi
