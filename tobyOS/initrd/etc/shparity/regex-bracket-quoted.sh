# Inside a bracket expression of a `=~` regex a quoted byte is itself --
# a backslash in front of it would be a member in ERE: `["."]` does not
# match `\`, `[']']` matches `]` -- tsh backslashed quoted bytes there too
# (finding 487). Outside brackets a quoted byte stays literal.
exec 2>&1
[[ '\' =~ ["."] ]] && echo yes1 || echo no1
[[ '\' =~ [\.] ]] && echo yes2 || echo no2
[[ '\' =~ [\\] ]] && echo yes3 || echo no3
[[ ']' =~ [']'] ]] && echo yes4 || echo no4
[[ a =~ ['a]'] ]] && echo yes5 || echo no5
[[ '\' =~ [^]"."] ]] && echo yes6 || echo no6
[[ x. =~ x["."] ]] && echo yes7 || echo no7
[[ b =~ ['[:alpha:]'] ]] && echo yes8 || echo no8
[[ 'a]' =~ ^[a]"]"$ ]] && echo yes9 || echo no9
[[ abc =~ "a.c" ]] && echo yes10 || echo no10
