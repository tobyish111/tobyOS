# Under lastpipe the last stage runs in the shell, so its `exit` ends the
# shell: `exit 142 | exit 14` ends the script with 14 -- tsh went on, 0
# (finding 517). Without lastpipe the stage is a subshell.
exec 2>&1
"$BASH" -c 'shopt -s lastpipe; exit 142 | exit 14; echo after: $?'; echo "rc=$?"
"$BASH" -c 'shopt -s lastpipe; true | exit 3; echo after'; echo "rc=$?"
"$BASH" -c 'true | exit 3; echo after $?'; echo "rc=$?"
"$BASH" -c 'shopt -s lastpipe; f() { true | exit 5; echo in-f; }; f; echo after'; echo "rc=$?"
