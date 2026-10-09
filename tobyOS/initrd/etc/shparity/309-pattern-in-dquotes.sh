# A PATTERN INSIDE DOUBLE QUOTES IS A FRESH QUOTING CONTEXT: in
# "${v#PAT}", "${v%PAT}" and "${v/PAT/REP}" the quotes in PAT -- written
# there or in a default word inside it -- quote, and what is unquoted is a
# pattern. tsh expanded PAT as part of the double-quoted string, so
# ${u:-'a*'} kept its quotes as text and matched nothing. And bash's
# parser leaves $'...' in a default word inside double quotes as plain
# text: a pattern when nested in a pattern, quoted by an inner "..."
# (finding 112; logs/b18 patdq, dqansi).
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
echo "16 [${v#$'a*'}] [${v#$'a'}] [${v#$'\x61'}]"
echo "17 [${v#${u:-$'a*'}}] [${v#${u:-$'\x61*'}}] [${v#${u:-$'a'}}]"
echo "18 [${u:-$'a\x41'}] [${u:-$'a*'}]"
echo "19" ${v#${u:-$'a*'}} ${v#$'a*'}
echo "20 [${v/${u:-$'*'}/X}] [${v/$'*'/X}]"
echo "21 [${v#"${u:-$'a*'}"}] [${v#${u:-"$'a*'"}}]"
x=$'\x61'; echo "22 [${v#${u:-$x}}]"
echo end
