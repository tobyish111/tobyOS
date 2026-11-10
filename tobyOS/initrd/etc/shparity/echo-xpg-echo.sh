# shopt -s xpg_echo makes echo read backslash escapes without -e; -E still
# turns them off, and \c still ends the output (finding 425).
exec 2>&1
shopt -u xpg_echo; echo 'a\n\nb'
shopt -s xpg_echo; echo 'a\n\nb'; echo -e 'c\td'; echo -E 'e\tf'; echo -n 'g\c'; echo h
echo 'x\101y' "two\tthree"
