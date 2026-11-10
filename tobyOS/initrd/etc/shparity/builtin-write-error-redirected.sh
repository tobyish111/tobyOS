# A builtin that cannot write its output says "write error" on fd 2 as its
# own redirections left it: with fd 3 open only for reading,
# `echo foo 2>&1 >&3` puts the error in the pipe; tsh said it once fd 2 was
# put back, on its own stderr (finding 583).
exec 3</etc/passwd
echo foo 2>&1 >&3 | sed 's/^.*echo: /echo: /'
printf 'x\n' 2>&1 >&3 | sed 's/^.*printf: /printf: /'
echo bar 2>/dev/null >&3; echo "rc=$?"
"$BASH" -c 'echo baz 2>&1 >&3' | sed 's/^.*echo: /child echo: /'
