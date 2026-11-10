# A `${ }` inside double quotes has quotes of its own: a `"` in it opens a
# string there, and the reader read it as the outer one's end and glued
# the next lines on (bash's quote1.sub). Under set -o posix a single quote
# in such a `${ }` is an ordinary byte that cannot hide a `}` (bash's POSIX
# mode, posixexp2.tests); the walkers that read ahead of the parser -- the
# reader, the keyword searches, the subshell's `)`, the comment stripper
# -- learnt the same rules (finding 291).
test="weferfds'dsfsdf"
echo "'${test//"'"/}'"
echo "'${test//"'"/"'\\''"}'"
printf "%s\n" "'${test//"'"/\'\\'\'}'"
echo next
if true; then echo "${IFS+"'"}"; fi
x=abc; echo "${x:-`echo }`}"
(echo "in sub ${IFS+"'"}" )
echo "${IFS+foo "b\
ar" baz}"
set -o posix
(echo 1 ${IFS+'}'z}) 2>&1
(echo 2 "${IFS+'}'z}") 2>&1
(echo 3 "foo ${IFS+'bar} baz") 2>&1
(echo 7 "${IFS+"}"z}") 2>&1
(echo 11 "$(echo "${IFS+'}'z}")") 2>&1
key=value; (printf '%s\n' "24 ${IFS+'$key'}") 2>&1
(echo -n '28 '; printf '%s\n' "${IFS+"'"x ~ x'}'x"'}"x}" #') 2>&1
u=x; (echo -n '29 '; printf '<%s> ' foo ${IFS+a"b$u{ {"{ {\}b} c ${IFS+d{}} bar ${IFS-e{}} baz; echo .) 2>&1
foo="x'a'y"; (echo 39 ${foo%*'a'*} / "${foo%*'a'*}" .) 2>&1
