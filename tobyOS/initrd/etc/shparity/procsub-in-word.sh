# `<(cmd)` inside a word is expanded there, as bash's lexer reads it: tsh
# ended the word at the `<`, so `f=<(cmd)` was the assignment `f=` and a
# command named /dev/fd/63 (bash's procsub.tests; finding 291).
f=<(echo hi); case $f in /dev/fd/*) echo "f is a path";; *) echo "f=[$f]";; esac
g=a<(true); case $g in a/dev/fd/*) echo "g glued";; *) echo "g=[$g]";; esac
echo x=<(true) y<(true) | sed 's|/dev/fd/[0-9]*|P|g'
h=<(echo there) eval 'cat $h'
eval f=<(echo test4) "; cat \$f"
declare d=<(true); case $d in /dev/fd/*) echo "declared";; esac
echo "a"<(true)'b' | sed 's|/dev/fd/[0-9]*|P|'
