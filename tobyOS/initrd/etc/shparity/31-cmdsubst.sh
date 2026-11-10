# POSIX 2.6.3 -- command substitution, both spellings.
# Trailing newlines are stripped; interior ones are not. Unquoted results are
# field-split, quoted ones are not -- the distinction that decides whether a
# multi-line result becomes one argument or many.
echo "a=[$(echo simple)]"
echo "b=[`echo backtick`]"
echo "c=[$(echo one; echo two)]"
echo "d=[$(printf 'x\n\n\n')]"
echo "e=[$(echo nested $(echo inner))]"
echo "f=[$(echo "quoted inside")]"

count() { echo "n=$#"; }
count $(echo a b c)
count "$(echo a b c)"
count $(echo one; echo two)
count "$(echo one; echo two)"
count $(true)
count "$(true)"

v=$(echo assigned)
echo "g=[$v]"
echo "h=[$(echo $v)]"

echo "status=$(false; echo $?)"
if [ "$(echo yes)" = yes ]; then echo cond-ok; fi
for w in $(echo p q r); do echo "iter:$w"; done
echo end
