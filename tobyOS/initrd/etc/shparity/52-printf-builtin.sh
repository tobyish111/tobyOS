# POSIX XCU printf -- the portable replacement for `echo -e`, and the one
# output builtin whose behaviour is fully specified. The format is REUSED
# until the arguments run out, which is what makes `printf '%s\n' "$@"` the
# idiom for one-item-per-line.
printf 'plain\n'
printf '%s\n' one
printf '%s\n' one two three
printf '%s %s\n' a b c d
printf '[%s]\n' ""
printf '%d\n' 42
printf '%d %d\n' 1 2
printf '%c%c\n' a b
printf '100%%\n'
printf 'a\tb\n'
printf 'no-newline'
printf '\n'
printf '%s' no-trailing
printf '\n'
printf '%s-%s\n' x
printf '%d\n' 0
printf '%s\n' 'literal $notexpanded'
printf '%s\n' "expanded-$USERVAR"
v=val
printf 'var=%s\n' "$v"
printf '%s\n' "a b"
printf '%s %s\n' "a b" "c d"
set -- p q
printf '%s\n' "$@"
printf 'multi\nline\n'
printf '%s\n' * 2>/dev/null | head -1 > /dev/null
printf 'done\n'
echo end
