# fc with history off: its own line is not in the list, so it counts from
# the newest entry -- `set +o history` itself, when that went in -- and the
# command it runs is recorded all the same; -s puts it in place of the
# newest entry, and -e first takes the newest out when the last line read
# with history on went in, counting its offsets as they were. tsh took
# that last line for fc's own and recorded nothing (finding 598).
f=$(mktemp)
run() {
    printf '%s\n' 'unset HISTFILE' 'set -o history' "$@" 'set -o history' 'fc -l' > "$f"
    "$BASH" "$f" 2>&1
    echo --
}
run 'echo a' 'echo b' 'set +o history' 'fc -s b=c'
run 'echo a' 'echo b' 'set +o history' 'fc -s echo'
run 'echo a' 'echo b' 'set +o history' 'fc -e cat -2'
run "HISTIGNORE='set*'" 'echo a' 'echo b' 'set +o history' 'fc -e cat -1'
run "HISTIGNORE='set*'" 'echo a' 'echo b' 'set +o history' 'fc -s b=c'
run "HISTIGNORE='echo b'" 'echo a' 'echo b' 'true c' 'set +o history' 'fc -e cat -1'
run 'echo a' 'echo b' 'set +o history' 'fc -e cat -1' 'fc -e cat -1'
rm -f "$f"
