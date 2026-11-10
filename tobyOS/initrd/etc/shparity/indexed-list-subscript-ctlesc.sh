# A ^A in an indexed list's subscript is bash's quoting byte and goes
# before the arithmetic reads it: [$'x\001y'] is xy, 0. A ^B or ^? stays.
# tsh called the ^A an invalid arithmetic operator (finding 567).
exec 2>&1
unset a; a=( [$'x\001y']=foo ); echo "rc=$?"; declare -p a
unset a; a=( [$'1\001']=b [2]=c ); declare -p a
unset a; a=( [$'x\001y\177z']=d ); echo "rc=$?"
echo end
