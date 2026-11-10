# `!` with nothing after it negates a NULL command (bash's BANG
# list_terminator): `!` is 1 and `! !` is 0, whatever $? was, and neither
# runs a command called `!`. tsh ran one ("!: command not found", 127) --
# its token loop compared the word with "!" while the tokenizer's marks
# were still on it -- and judged `if !; then` a syntax error.
exec 2>&1
!
echo "bang: $?"
! !
echo "bang bang: $?"
! ! true
echo "bang bang true: $?"
true; !; echo "after true, bang: $?"
false; ! ; echo "after false, bang: $?"
eval '! !'; echo "eval bang bang: $?"
if !; then echo then; else echo else; fi
{ if time; then echo "time alone: then"; fi; } 2>/dev/null
while !; do echo never; done; echo "while: $?"
v='!'; $v 2>/dev/null; echo "expanded bang is a word: $?"
"!" 2>/dev/null; echo "quoted bang is a word: $?"
echo done
