# This is odd because the variable is populated successfully.  OSH/YSH might
# need a separate put reading feature that doesn't use IFS.

echo -n ZZZ | { read x; echo status=$?; echo $x; }
