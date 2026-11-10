# A case command's patterns are not command words, so an alias of the same
# name stays a pattern: `pat)` on the line after `in`, after `;;`, or as
# `(pat)` -- tsh replaced it and the case fell through (finding 468).
exec 2>&1
shopt -s expand_aliases
alias pat='other' q2=zzz
w=pat
case "$w"
in
pat) echo first;;
*) echo none1;;
esac
w=q2
case "$w" in
pat) echo one;;
q2) echo two;;
*) echo none2;;
esac
w=pat
case $w in (pat) echo paren;; *) echo none3;; esac
case $w in
(pat) echo paren2;;
esac
case $w in x) case y in pat) echo no;; y) echo inner;; esac;; pat) echo outer;; esac
