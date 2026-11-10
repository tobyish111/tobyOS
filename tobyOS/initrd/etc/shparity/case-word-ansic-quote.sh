# A case word is skipped to its `in` with `$'...'` read as ANSI-C quoting,
# where `\'` is a quote inside it: `case $'w\'' in` has the word w' -- tsh
# read '...' closed at the `\'` and found no `in` (finding 465, after 442).
exec 2>&1
case $'w\'' in "w'") echo yes;; *) echo no;; esac
case $'a;\'b' in "a;'b") echo yes2;; *) echo no2;; esac
case $'\\'x in '\x') echo yes3;; *) echo no3;; esac
