set -- ${u:-a\ b}; echo "1: $#"
IFS=:; set -- ${u:-p\:q}; echo "2: $#"; IFS=' '
set -- ${u:-\*}; echo "3: $# $1"
x=$'a\tb'; set -- ${u:-c\ d$x}; echo "4: $#"
case 'a b' in a\ b) echo "5: case ok";; esac
