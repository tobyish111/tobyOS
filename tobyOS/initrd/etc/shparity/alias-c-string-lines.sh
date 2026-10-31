# A -c string that ends in an alias of several lines runs every line: the
# first, a program, is not the string's last command -- tsh ran it in the
# shell's place and the other lines never ran (finding 473).
exec 2>&1
"$BASH" -c "shopt -s expand_aliases
alias ml='/bin/echo first
echo second'
ml"
"$BASH" -c "shopt -s expand_aliases
alias ml='echo one
/bin/echo two'
ml"
