# bash READS A WHOLE COMMAND BEFORE IT RUNS ANY OF IT, and one that does
# not parse runs not at all: bash names the first token its grammar cannot
# take, echoes the line, and $? is 2. tsh ran what it could and complained
# in its own words -- "if: expected '; fi'", `a &&` ran a, `;` was status 0.
# Each case is eval'd so the script runs on; stderr is folded in. Then the
# places other than eval: an array literal (its line is dropped, $? 1), a
# sourced file (ends the file, status 2), `...` (ends the substitution),
# bash's [[ ]] state leak, and last a script-level error, which ends this.
exec 2>&1
t() { eval "$1"; echo "  [$1] status=$?"; }
t 'if then'
t 'if true; then echo; fi fi'
t 'done'
t 'fi'
t 'then'
t 'esac'
t ')'
t '}'
t 'if true; then'
t 'while true; do'
t 'for x in a; do echo'
t 'case x in'
t 'case x in a) echo;;'
t 'for'
t 'for x in'
t 'while'
t '{'
t '{ echo'
t '( echo'
t 'echo $('
t 'echo "unterminated'
t "echo 'unterminated"
t 'echo `unterminated'
t 'a &&'
t '&& b'
t '| x'
t 'x |'
t 'x ||'
t 'x ;;'
t ';'
t ';;'
t 'echo >'
t 'echo <'
t 'echo > ;'
t 'f() '
t 'function'
t '[[ a'
t '[[ ]]'
t 'x=(1 2'
t 'echo ${'
t 'echo $((1+'
t '((1+'
t 'case x in a) ;; esac esac'
t 'if true; then echo; else; fi'
t 'while do done'
t 'echo a b ) c'
t 'select'
t 'time |'
t 'echo in; fi'
t 'echo (x)'
t 'a=b() { :; }'
t 'for ((i=0; i<2)); do :; done'
t '[[ -f ]]'
t '[[ a == ]]'
t '[[ ( a ]]'
t '[[ a ) ]]'
t 'cat <<EOF >'
x=(1 & 2)
echo "array literal: $?"
printf 'echo lib1\nfi\necho lib3\n' > /tmp/fx183-lib.sh
. /tmp/fx183-lib.sh
echo "source: $?"
echo "bq: [`fi`]"
eval "[[ a b ]]"
eval "[[ x ]]"; echo "leaked: $?"
eval "[[ x ]]"; echo "reset: $?"
rm -f /tmp/fx183-lib.sh
echo "last line"
if true; then echo never; fi fi
echo "not reached"
