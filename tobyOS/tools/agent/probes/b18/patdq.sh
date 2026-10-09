# patdq.sh: a pattern inside double quotes (finding 112) -- ${v#PAT},
# ${v%PAT}, ${v/PAT/REP} written in "...", with the quotes in PAT direct and
# in a default word inside it; and the same outside quotes.
exec 2>&1
v='a*b'; w='*a*b*'
echo "1 [${v#'a*'}] [${v#"a*"}] [${v#a*}]"
echo "2 [${v#${u:-'a*'}}] [${v#${u:-"a*"}}] [${v#${u:-a*}}]"
echo "3 [${v%${u:-'*b'}}] [${v%${u:-*b}}] [${v%%${u:-'*'}*}]"
echo "4 [${v/${u:-'*'}/X}] [${v/${u:-*}/X}] [${v//${u:-'*'}/X}]"
echo "5 [${w//${u:-'*'}/-}] [${w/#${u:-'*'}/-}] [${w/%${u:-'*'}/-}]"
echo "6 [${v/'*'/X}] [${v/"*"/X}] [${v/\*/X}]"
echo "7 [${v#"${u:-a*}"}] [${v#"${u:-'a*'}"}]"
echo "8 [${u:-'a*'}] [${u:-"a*"}] [${u:-a*}]"
echo "9 [${v/${u:-'*'}/${u:-'R'}}] [${v/a/${u:-'R*'}}]"
u='*'; echo "10 [${v#$u}] [${v#"$u"}] [${v#${u}}]"
unset u
x=${v#${u:-'a*'}}; echo "11 [$x]"
y="${v#${u:-'a*'}}"; echo "12 [$y]"
echo "13" ${v#${u:-'a*'}} ${v/${u:-'*'}/X}
case "${v#${u:-'a'}}" in '*b') echo "14 star-b";; *) echo "14 other";; esac
echo "15 [${v#${u:-$'a*'}}] [${v#${u:-\a\*}}]"
echo end
