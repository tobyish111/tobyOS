# A dup to a descriptor that is not open names the word as written on the
# default descriptor -- `>&$a` is "$a: Bad file descriptor", `<&$a` too --
# and the descriptor redirected otherwise, `2>&$a` "2"; tsh named the
# target's number (finding 451).
exec 2>&1
a=42; echo foo >&$a; echo foo >&42; echo foo >&$(echo 42); echo foo >&"$a"; echo foo 2>&$a; cat <&$a
exec {fd}<>/dev/null; echo r >&$fd; exec {fd}>&-; echo s >&$fd
