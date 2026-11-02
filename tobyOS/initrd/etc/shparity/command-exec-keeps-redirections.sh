# `command exec 2>f` is exec: its redirections stay in the shell, after
# any number of `command` words and -p; `builtin exec`'s are a builtin's
# only. A function called exec is what `exec 3>&-` runs outside posix
# mode, and `command exec` passes it by -- tsh undid command exec's
# redirections at its end and ran the builtin over the function (finding
# 586).
exec 2>&1
d=$(mktemp -d); cd "$d" || exit
exec 9>&2
command exec 2>f; echo whatsis >&2; exec 2>&9 9>&-
echo "f: $(cat f)"
command command -p -- exec 3>g; echo three >&3; exec 3>&-; echo "g: $(cat g)"
builtin exec 4>h; echo four 2>/dev/null >&4; st=$?; echo "h: [$(cat h)] $st"
exec() { echo "fn exec $*"; }
command exec 3>i; echo eye >&3; exec 3>&-; echo "i: $(cat i)"
command exec 3>&-
unset -f exec
cd /; rm -rf "$d"
