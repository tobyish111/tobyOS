# OSH bug fix.  ErrExit needs a counter, not a boolean.
set -o errexit
if { ! false; false; true; } then
  echo true
fi
false
echo done
