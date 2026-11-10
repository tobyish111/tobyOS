# Notes:
# - OSH prompt goes to stdout and bash goes to stderr
# - This test seems to fail on the system bash, but succeeds with spec-bin/bash
echo 'echo foo' | PS1='[prompt] ' $SH --rcfile /dev/null -i >out.txt 2>err.txt
fgrep -q '[prompt]' out.txt err.txt
echo match=$?
