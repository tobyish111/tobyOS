trap 'echo line=$LINENO' ERR

( false; echo subshell )

x=$( false; echo command sub )

false & wait

{ false; echo async; } & wait

false
echo ok
