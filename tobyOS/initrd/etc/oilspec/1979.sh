# TODO: we can add more of these

f() ( echo 'subshell body' )

code=$(typeset -f f)

$SH -c "$code; f"
