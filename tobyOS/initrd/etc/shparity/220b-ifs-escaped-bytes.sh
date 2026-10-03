# An escaped or quoted IFS character in a word or an array literal is data,
# not a delimiter: `IFS=: a=(x\:y)` is one element. tsh split it.
# (batch 12; probe p2 in C:\t\b12)
exec 2>&1
set -- ${u:-a\ b}; echo "1: $#"
IFS=:; set -- ${u:-p\:q}; echo "2: $#"; IFS=' '
set -- ${u:-\*}; echo "3: $# $1"
x=$'a\tb'; set -- ${u:-c\ d$x}; echo "4: $#"
case 'a b' in a\ b) echo "5: case ok";; esac
