# A word after `<&` or `>&` that is not a descriptor, as bash's redir.c
# takes it: empty is descriptor -1, "Bad file descriptor" (naming the word
# as written, or the descriptor when it is not 0 for <& or 1 for >&);
# after `>&` on fd 1 it is a file taking stdout AND stderr; anything else
# is "WORD: ambiguous redirect"; `M-` moves only as written. tsh opened a
# file for every one of them, and gave `1>&file` stdout alone.
# (batch 12; probes p109 p110 in C:\t\b12)
exec 2>&1
x=
read -r a <&"$x"; echo "1 rc=$?"
read -r a <&""; echo "2 rc=$?"
cat <&foo; echo "3 rc=$?"
echo hi >&"$x"; echo "4 rc=$?"
echo hi 1>&""; echo "5 rc=$?"
echo hi >&p255out.txt; echo "6 rc=$? $(cat p255out.txt)"; rm -f p255out.txt
cat 0<&/etc/hostname >/dev/null; echo "8 rc=$?"
y="1 2"; echo hi >&$y; echo "9 rc=$?"
echo hi 2>&"$x"; echo "10 rc=$?"
exec 3<&""; echo "11 rc=$?"
{ echo out; echo err >&2; } 1>&p255a; echo "12 rc=$? [$(tr '\n' ' ' < p255a)]"
{ echo out; echo err >&2; } >&p255b; echo "13 rc=$? [$(tr '\n' ' ' < p255b)]"
echo hi 2>&p255c; echo "14 rc=$?"; ls p255c 2>&1 | sed 's/^/14 ls: /'
echo hi 3>&p255d; echo "15 rc=$?"
z=7-; echo hi >&$z; echo "16 rc=$? [$(cat 7- 2>&1)]"
rm -f p255a p255b p255c p255d 7-
