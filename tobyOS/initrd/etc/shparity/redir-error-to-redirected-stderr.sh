# A redirection's failure is reported on fd 2 as the command's earlier
# redirections left it: `echo foo 2>&1 >&$a` puts "$a: Bad file
# descriptor" in the pipe, and 2>/dev/null before it silences it -- tsh
# wrote it to its own stderr (finding 455).
exec 2>&1
a=42
echo foo 2>&1 >&$a | sed 's/^.*line [0-9]*: /piped: /'
echo foo 2>/dev/null >&$a; echo "rc=$?"
echo foo 2>&1 >/nonexistent/x | sed 's/^.*line [0-9]*: /piped2: /'
