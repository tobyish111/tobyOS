# <(cmd) in the word of ${x:-WORD} and its kin, unquoted, runs as it does on
# the command line: cat ${u:-<(echo a)} reads a, and x=${u:-<(echo d)} holds
# a /dev/fd path; inside double quotes the text stays (finding 411).
exec 2>&1
foo=; cat ${foo:-<(echo a)}; echo "${foo:-<(echo c)}"
x=${foo:-<(echo d)}; echo "${x:0:8}"
foo=1; cat ${foo:+<(echo e)}; unset u; cat ${u=<(echo f)}; echo "${u:0:8}"
