# "command not found" and cd's error show a name that holds a byte which is
# no printable character as $'...': `$'a\tb': command not found` -- tsh
# wrote the bytes (finding 497). A printable name is shown as it is.
exec 2>&1
export LANG=en_US.UTF-8
p=$'\065\247\100\063'
"$p"
cd "$p"
q=$'a\tb'
"$q"
cd "$q"
r=$'caf\303\251'
"$r"
cd "$r"
