# A prefix assignment goes into the command's temporary environment as it
# is written: an -i variable's value is not evaluated there, nor is an -u
# or -l one's case changed -- `n=1+ true` is no error, and `n=3+4 eval
# 'echo $n'` says 3+4. tsh evaluated it for a builtin, and so erred and
# threw the line away (finding 632).
exec 2>&1
declare -i n=5
n=1+ true; echo "same line: $?"
n=3+4 eval 'echo "eval sees $n"'
f() { echo "f sees $n"; }
n=6+7 f
echo "after: $n"
declare -u U
U=abc eval 'echo "eval sees $U"'
declare -l L=X
L=ABC command eval 'echo "command eval sees $L"'
n+=2 eval 'echo "append: $n"'
