# `command NAME` runs NAME without looking for a function, so with a
# redirection whose word fails to expand it is an external command's
# failure -- status 1, the line going on -- whenever NAME is no builtin,
# even when a function has NAME's name; a builtin, and `command -v`, keep
# the line's discard. tsh took `command`, a builtin, and discarded the line
# (finding 642).
exec 2>&1
command cat <<< ${a[}; echo "command cat: $?"
command -p cat <<< ${a[}; echo "command -p cat: $?"
f() { cat; }; command f <<< ${a[}; echo "command f: $?"
command -- cat <<< ${a[}; echo "command -- cat: $?"
command echo hi > ./out${a[}; echo "not after command echo"
echo "after command echo: $?"
command -v cat <<< ${a[}; echo "not after command -v"
echo "after command -v: $?"
