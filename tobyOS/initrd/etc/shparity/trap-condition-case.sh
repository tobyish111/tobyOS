# trap takes its own conditions -- EXIT DEBUG RETURN ERR -- in any case,
# as it does the signals': `trap - debug` -- tsh said "bad condition
# 'debug'" (finding 512).
exec 2>&1
trap 'echo D' debug
trap -p DEBUG
trap - debug
trap 'echo X' exit
trap -p EXIT
trap - Exit
trap 'echo R' return; trap -p RETURN; trap - return
trap 'echo E' err; trap -p ERR; trap - ERR
trap
