# A function whose body is a subshell keeps the redirections written after
# it: `f() ( echo c >&2 ) 2>/dev/null` sends c nowhere, and type shows
# them -- tsh dropped them (finding 504).
exec 2>&1
fa() ( echo a ) 2>&1
fc() ( echo c >&2 ) 2>/dev/null
fd() ( echo d ) >/dev/null
type fa fc
fa; fc; fd; echo "rc=$?"
