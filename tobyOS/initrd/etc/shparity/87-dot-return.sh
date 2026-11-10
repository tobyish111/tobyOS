# POSIX dot (.): `return` inside a dot-script ends the dot-script, hands
# its status to the caller, and the caller CONTINUES after the dot. A dot
# on a missing file is an error the shell survives (bash non-interactive).
printf 'echo in-script\nreturn 7\necho not-reached\n' > dot87.sh
. ./dot87.sh
echo "1=$?"
printf 'echo second\n' > dot87b.sh
. ./dot87b.sh
echo "2=$?"
. ./dot87-missing.sh
echo "3=$?"
echo end
