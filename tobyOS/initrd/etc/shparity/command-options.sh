# command's options are -p -v -V, combined as they like, and -- ends them;
# any other letter is "command: -i: invalid option" and the usage line,
# status 2, where tsh ran a command called -i (finding 420).
exec 2>&1
command -i "echo x"; echo "rc=$?"; command -x; echo "rc=$?"
command -pv echo; echo "rc=$?"; command -- echo ok; echo "rc=$?"
