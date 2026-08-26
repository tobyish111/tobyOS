# ALL FOUR SHELLS behave differently here!
#
# bash chooses to swallow the error!  We agree with zsh if ANY word lookup
# fails, then the whole thing fails.

myfunc() { echo x; }
command -v echo myfunc ZZZ for
echo status=$?
