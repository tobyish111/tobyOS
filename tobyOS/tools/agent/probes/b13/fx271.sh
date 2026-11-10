# A CARRIAGE RETURN IS TEXT, as bash has it: not a blank, not a line end, not
# a command separator. A CRLF script's last words carry the \r -- `x=1\r`
# is "1\r", `then\r` is no keyword -- and `eval "echo q\rw"` is ONE command.
# tsh read a CR as a newline: CRLF scripts ran where bash's break, and a CR
# inside a line split it into two commands (`w` ran) (logs/b13 q55).
exec 2>&1
cd /tmp
cr=$'\r'
eval "x=a${cr}b"; printf '%q\n' "$x"
eval "printf '%q\n' q${cr}w"
printf 'echo one\r\necho two\n' > cr271.sh
"$BASH" cr271.sh | while IFS= read -r l; do printf '%q\n' "$l"; done
printf 'x=1\r\necho "[${#x}]"\n' > cr271.sh
"$BASH" cr271.sh
printf 'if true\r\nthen echo y\r\nfi\r\n' > cr271.sh
"$BASH" cr271.sh 2>&1 | sed "s#^[^:]*cr271.sh#SCRIPT#"
echo "status=${PIPESTATUS[0]}"
printf 'echo a\rb c\n' > cr271.sh
"$BASH" cr271.sh 2>&1 | sed "s#^[^:]*cr271.sh#SCRIPT#" | while IFS= read -r l; do printf '%q\n' "$l"; done
printf 'case a in\r\na) echo m;;\r\nesac\r\n' > cr271.sh
"$BASH" cr271.sh 2>&1 | sed "s#^[^:]*cr271.sh#SCRIPT#" | while IFS= read -r l; do printf '%q\n' "$l"; done
echo "status=${PIPESTATUS[0]}"
printf 'echo s\r\n' > cr271.sh
. ./cr271.sh | while IFS= read -r l; do printf '%q\n' "$l"; done
v=$(printf 'a\r'); echo "${#v}"
[[ a$cr == a ]] && echo eq || echo ne
echo $(( 1 ${cr}+ 1 )) 2>&1 | sed 's/^[^:]*: line [0-9]*: //'
rm -f cr271.sh
