# A command substitution's status is $? from there on, inside the same
# command: `false; echo "$(true) $?"` prints 0, a later word sees the last
# one, and a function called with `f $(exit 5)` starts with $? 5 -- but a
# $( ) in the prompt leaves $? alone. tsh kept the last command's status
# until the next one ran (finding 589).
false; echo "$(true) $?"
true; echo $(false) $?
true; echo "$(exit 3)" "$?" "$(exit 4)$?"
false; echo "`true`$?" "$(true)$(( $? + 10 ))"
false; x="$(true)$?"; echo "x=$x"
f() { echo "in=$?"; }; true; f $(exit 5)
printf '%s\n' "PS1='\$(exit 7)> '" 'false' 'echo "st=$?"' |
    "$BASH" --norc -i 2>&1 | grep '^st='
